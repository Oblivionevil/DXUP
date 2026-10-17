#pragma once

#include "../directx/directx_backend.h"
#include "../util/util_config.h"
#include "../util/util_types.h"
#include "../util/com_private_data.h"
#include "d3d11_d3d12_bridge.h"
#include "d3d11_backend_device.h"
#include "d3d11_view_validation.h"
#include "d3d11_shader_validation.h"
#include "d3d11_shared_resource.h"

#include <d3d11_4.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <d3d11on12.h>

#include "d3d10_device.h"

namespace dxup {

  // Forward declarations
  class D3D11Device;
  class D3D11ImmediateContext;
  class DXGIDevice;

  inline DXGI_FORMAT resolveImageViewFormat(DXGI_FORMAT viewFormat, DXGI_FORMAT resourceFormat) {
    return viewFormat == DXGI_FORMAT_UNKNOWN ? resourceFormat : viewFormat;
  }

  inline DXGI_FORMAT resolveDepthStencilFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_D32_FLOAT;
      case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
      case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_D16_UNORM;
      case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      default:                            return format;
    }
  }

  inline uint64_t hashShaderBytecode(const void* bytecode, size_t bytecodeSize) {
    if (!bytecode || !bytecodeSize)
      return 0ull;

    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    for (size_t i = 0; i < bytecodeSize; i++) {
      hash ^= static_cast<uint64_t>(bytes[i]);
      hash = fnv64Multiply(hash);
    }
    return hash;
  }

  inline void appendStableHash(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
      hash ^= static_cast<uint64_t>(bytes[i]);
      hash = fnv64Multiply(hash);
    }
  }

  inline uint64_t hashInputLayout(const std::vector<InputElementDesc>& elements) {
    uint64_t hash = 1469598103934665603ull;
    const uint64_t elementCount = static_cast<uint64_t>(elements.size());
    appendStableHash(hash, &elementCount, sizeof(elementCount));

    for (const auto& element : elements) {
      const size_t semanticLength = element.semanticName
        ? std::strlen(element.semanticName)
        : 0;
      appendStableHash(hash, &semanticLength, sizeof(semanticLength));
      if (semanticLength)
        appendStableHash(hash, element.semanticName, semanticLength);
      appendStableHash(hash, &element.semanticIndex, sizeof(element.semanticIndex));
      appendStableHash(hash, &element.format, sizeof(element.format));
      appendStableHash(hash, &element.inputSlot, sizeof(element.inputSlot));
      appendStableHash(hash, &element.byteOffset, sizeof(element.byteOffset));
      appendStableHash(hash, &element.inputClass, sizeof(element.inputClass));
      appendStableHash(hash, &element.instanceDataStepRate, sizeof(element.instanceDataStepRate));
    }

    return hash;
  }

  inline std::string summarizeShaderBindings(const void* bytecode, size_t bytecodeSize) {
    if (!bytecode || !bytecodeSize)
      return "bytecode-empty";

    HMODULE compilerModule = LoadLibraryA("d3dcompiler_47.dll");
    if (!compilerModule)
      compilerModule = LoadLibraryA("d3dcompiler_43.dll");
    if (!compilerModule)
      return "reflect-unavailable";

    using PFN_D3DReflect = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto d3dReflect = reinterpret_cast<PFN_D3DReflect>(GetProcAddress(compilerModule, "D3DReflect"));
    if (!d3dReflect) {
      FreeLibrary(compilerModule);
      return "reflect-missing";
    }

    ID3D11ShaderReflection* reflection = nullptr;
    HRESULT hr = d3dReflect(bytecode, bytecodeSize, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection) {
      FreeLibrary(compilerModule);
      return "reflect-failed";
    }

    D3D11_SHADER_DESC shaderDesc = {};
    hr = reflection->GetDesc(&shaderDesc);
    if (FAILED(hr)) {
      reflection->Release();
      FreeLibrary(compilerModule);
      return "reflect-desc-failed";
    }

    std::string summary;
    char item[160] = {};
    for (UINT i = 0; i < shaderDesc.BoundResources; i++) {
      D3D11_SHADER_INPUT_BIND_DESC bindDesc = {};
      if (FAILED(reflection->GetResourceBindingDesc(i, &bindDesc)))
        continue;

      const char* typeName = "?";
      switch (bindDesc.Type) {
        case D3D_SIT_CBUFFER:                  typeName = "cb"; break;
        case D3D_SIT_TBUFFER:                  typeName = "tb"; break;
        case D3D_SIT_TEXTURE:                  typeName = "t"; break;
        case D3D_SIT_SAMPLER:                  typeName = "s"; break;
        case D3D_SIT_UAV_RWTYPED:              typeName = "u"; break;
        case D3D_SIT_STRUCTURED:               typeName = "t-struct"; break;
        case D3D_SIT_BYTEADDRESS:              typeName = "t-byte"; break;
        case D3D_SIT_UAV_RWSTRUCTURED:         typeName = "u-struct"; break;
        case D3D_SIT_UAV_RWBYTEADDRESS:        typeName = "u-byte"; break;
        case D3D_SIT_UAV_APPEND_STRUCTURED:    typeName = "u-append"; break;
        case D3D_SIT_UAV_CONSUME_STRUCTURED:   typeName = "u-consume"; break;
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
          typeName = "u-counter";
          break;
        default:
          break;
      }

      if (!summary.empty())
        summary += ",";

      std::snprintf(
        item,
        sizeof(item),
        "%s%u[%u]=%s",
        typeName,
        bindDesc.BindPoint,
        bindDesc.BindCount,
        bindDesc.Name ? bindDesc.Name : "?");
      summary += item;
    }

    reflection->Release();
    FreeLibrary(compilerModule);
    return summary.empty() ? "no-bound-resources" : summary;
  }

  inline std::string summarizeVertexShaderInputs(const void* bytecode, size_t bytecodeSize) {
    if (!bytecode || !bytecodeSize)
      return "bytecode-empty";

    HMODULE compilerModule = LoadLibraryA("d3dcompiler_47.dll");
    if (!compilerModule)
      compilerModule = LoadLibraryA("d3dcompiler_43.dll");
    if (!compilerModule)
      return "reflect-unavailable";

    using PFN_D3DReflect = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto d3dReflect = reinterpret_cast<PFN_D3DReflect>(GetProcAddress(compilerModule, "D3DReflect"));
    if (!d3dReflect) {
      FreeLibrary(compilerModule);
      return "reflect-missing";
    }

    ID3D11ShaderReflection* reflection = nullptr;
    HRESULT hr = d3dReflect(bytecode, bytecodeSize, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection) {
      FreeLibrary(compilerModule);
      return "reflect-failed";
    }

    D3D11_SHADER_DESC shaderDesc = {};
    hr = reflection->GetDesc(&shaderDesc);
    if (FAILED(hr)) {
      reflection->Release();
      FreeLibrary(compilerModule);
      return "reflect-desc-failed";
    }

    std::string summary;
    char item[160] = {};
    for (UINT i = 0; i < shaderDesc.InputParameters; i++) {
      D3D11_SIGNATURE_PARAMETER_DESC paramDesc = {};
      if (FAILED(reflection->GetInputParameterDesc(i, &paramDesc)))
        continue;

      if (!summary.empty())
        summary += ",";

      std::snprintf(
        item,
        sizeof(item),
        "%s%u:r%u mask0x%x sv=%u",
        paramDesc.SemanticName ? paramDesc.SemanticName : "?",
        paramDesc.SemanticIndex,
        paramDesc.Register,
        paramDesc.Mask,
        static_cast<uint32_t>(paramDesc.SystemValueType));
      summary += item;
    }

    reflection->Release();
    FreeLibrary(compilerModule);
    return summary.empty() ? "no-inputs" : summary;
  }

  inline uint64_t getTrackedBufferShadowSize(const D3D11_BUFFER_DESC& desc) {
    // Buffer shadows only feed the optional final-pass diagnostics below.
    // Keeping them enabled in normal gameplay duplicates every mapped constant
    // buffer on Unmap, which can dominate CPU time without affecting rendering.
    if (!d3d11InfoLoggingEnabled())
      return 0u;

    if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
      return desc.ByteWidth;

    const UINT trackedIaFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    if (desc.BindFlags & trackedIaFlags)
      return (std::min)(uint64_t(desc.ByteWidth), uint64_t(512u));

    return 0u;
  }

  inline bool isBlockCompressedFormat(DXGI_FORMAT format) {
    return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM)
        || (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
  }

  inline bool isTypelessImageFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      case DXGI_FORMAT_R32G32B32_TYPELESS:
      case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      case DXGI_FORMAT_R32G32_TYPELESS:
      case DXGI_FORMAT_R32G8X24_TYPELESS:
      case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      case DXGI_FORMAT_R16G16_TYPELESS:
      case DXGI_FORMAT_R32_TYPELESS:
      case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_R8G8_TYPELESS:
      case DXGI_FORMAT_R16_TYPELESS:
      case DXGI_FORMAT_R8_TYPELESS:
      case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC2_TYPELESS:
      case DXGI_FORMAT_BC3_TYPELESS:
      case DXGI_FORMAT_BC4_TYPELESS:
      case DXGI_FORMAT_BC5_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      case DXGI_FORMAT_BC6H_TYPELESS:
      case DXGI_FORMAT_BC7_TYPELESS:
        return true;
      default:
        return false;
    }
  }

  // Formats of a texture DXUP multisamples although the application does not
  // (dxup.conf d3d11.msaaForce): the typed format the multisampled copy is
  // resolved and refilled in, and for depth the format a shader reads depth
  // with. False for formats that cannot be resolved (integers) and for those
  // that are not rendered to.
  // HDR color formats, which can hold NaN and infinity. One- and
  // two-channel float targets mostly hold data (depth copies, vectors, bits
  // packed into floats) that the hardware resolve treats as it always did.
  inline bool isFloatColorFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R32G32B32A32_FLOAT:
      case DXGI_FORMAT_R32G32B32_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
      default:
        return false;
    }
  }

  // Formats engines keep motion vectors in (Unreal Engine 4: 16-bit UNORM).
  inline bool isMotionVectorFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R16G16_FLOAT:
      case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_SNORM:
      case DXGI_FORMAT_R16G16B16A16_UNORM:
        return true;
      default:
        return false;
    }
  }

  struct ForcedMsaaFormats {
    bool        depth = false;
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthRead = DXGI_FORMAT_UNKNOWN;
  };

  inline bool getForcedMsaaFormats(DXGI_FORMAT format, bool depthStencil, ForcedMsaaFormats& out) {
    out = {};
    if (depthStencil) {
      out.depth = true;
      switch (format) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
          out.typed = DXGI_FORMAT_D24_UNORM_S8_UINT;
          out.depthRead = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
          return true;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
          out.typed = DXGI_FORMAT_D32_FLOAT;
          out.depthRead = DXGI_FORMAT_R32_FLOAT;
          return true;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
          out.typed = DXGI_FORMAT_D16_UNORM;
          out.depthRead = DXGI_FORMAT_R16_UNORM;
          return true;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
          out.typed = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
          out.depthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
          return true;
        default:
          return false;
      }
    }
    switch (format) {
      // Typeless formats: the usual typed one until a render target view
      // names the format the application draws in.
      case DXGI_FORMAT_R32G32B32A32_TYPELESS: out.typed = DXGI_FORMAT_R32G32B32A32_FLOAT; return true;
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: out.typed = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
      case DXGI_FORMAT_R32G32_TYPELESS:       out.typed = DXGI_FORMAT_R32G32_FLOAT; return true;
      case DXGI_FORMAT_R10G10B10A2_TYPELESS:  out.typed = DXGI_FORMAT_R10G10B10A2_UNORM; return true;
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:     out.typed = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
      case DXGI_FORMAT_R16G16_TYPELESS:       out.typed = DXGI_FORMAT_R16G16_FLOAT; return true;
      case DXGI_FORMAT_R32_TYPELESS:          out.typed = DXGI_FORMAT_R32_FLOAT; return true;
      case DXGI_FORMAT_R8G8_TYPELESS:         out.typed = DXGI_FORMAT_R8G8_UNORM; return true;
      case DXGI_FORMAT_R16_TYPELESS:          out.typed = DXGI_FORMAT_R16_FLOAT; return true;
      case DXGI_FORMAT_R8_TYPELESS:           out.typed = DXGI_FORMAT_R8_UNORM; return true;
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:     out.typed = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
      case DXGI_FORMAT_B8G8R8X8_TYPELESS:     out.typed = DXGI_FORMAT_B8G8R8X8_UNORM; return true;
      // Resolvable typed formats.
      case DXGI_FORMAT_R32G32B32A32_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R16G16B16A16_UNORM:
      case DXGI_FORMAT_R16G16B16A16_SNORM:
      case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R11G11B10_FLOAT:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_R8G8B8A8_SNORM:
      case DXGI_FORMAT_R16G16_FLOAT:
      case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_SNORM:
      case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R8G8_UNORM:
      case DXGI_FORMAT_R8G8_SNORM:
      case DXGI_FORMAT_R16_FLOAT:
      case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_SNORM:
      case DXGI_FORMAT_R8_UNORM:
      case DXGI_FORMAT_R8_SNORM:
      case DXGI_FORMAT_A8_UNORM:
      case DXGI_FORMAT_B5G6R5_UNORM:
      case DXGI_FORMAT_B5G5R5A1_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      case DXGI_FORMAT_B4G4R4A4_UNORM:
        out.typed = format;
        return true;
      default:
        return false;
    }
  }

  // Integer formats average to nothing: their views draw single-sampled.
  inline bool isResolvableColorViewFormat(DXGI_FORMAT format) {
    ForcedMsaaFormats formats;
    return getForcedMsaaFormats(format, false, formats) && formats.typed == format;
  }

  inline DXGI_FORMAT validateImageViewFormat(DXGI_FORMAT viewFormat, DXGI_FORMAT resourceFormat) {
    DXGI_FORMAT resolvedFormat = resolveImageViewFormat(viewFormat, resourceFormat);

    if (resourceFormat == DXGI_FORMAT_UNKNOWN || isTypelessImageFormat(resourceFormat))
      return resolvedFormat;

    return resolvedFormat == resourceFormat ? resolvedFormat : resourceFormat;
  }

  inline uint32_t resolveMipLevelCount(uint32_t totalMipLevels, uint32_t baseMipLevel, uint32_t requestedMipLevels) {
    uint32_t effectiveMipLevels = totalMipLevels ? totalMipLevels : 1u;

    if (requestedMipLevels == 0 || requestedMipLevels == UINT(-1))
      return effectiveMipLevels > baseMipLevel ? effectiveMipLevels - baseMipLevel : 1u;

    return requestedMipLevels;
  }

  inline bool isBc1FingerprintFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC1_UNORM:
      case DXGI_FORMAT_BC1_UNORM_SRGB:
        return true;

      default:
        return false;
    }
  }

  inline bool isBc3FingerprintFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_BC3_TYPELESS:
      case DXGI_FORMAT_BC3_UNORM:
      case DXGI_FORMAT_BC3_UNORM_SRGB:
        return true;

      default:
        return false;
    }
  }

  inline bool isTrackedInitialTextureFingerprintFormat(DXGI_FORMAT format) {
    return isBc1FingerprintFormat(format) || isBc3FingerprintFormat(format);
  }

  inline uint8_t expand5To8Fingerprint(uint16_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
  }

  inline uint8_t expand6To8Fingerprint(uint16_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
  }

  struct FingerprintColorSample {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
  };

  inline void decodeBc1FingerprintPalette(const uint8_t* block, uint8_t palette[4][4], bool forceOpaqueFourColor) {
    uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));

    palette[0][0] = expand5To8Fingerprint((color0 >> 11) & 0x1f);
    palette[0][1] = expand6To8Fingerprint((color0 >> 5) & 0x3f);
    palette[0][2] = expand5To8Fingerprint(color0 & 0x1f);
    palette[0][3] = 255;

    palette[1][0] = expand5To8Fingerprint((color1 >> 11) & 0x1f);
    palette[1][1] = expand6To8Fingerprint((color1 >> 5) & 0x3f);
    palette[1][2] = expand5To8Fingerprint(color1 & 0x1f);
    palette[1][3] = 255;

    if (forceOpaqueFourColor || color0 > color1) {
      for (uint32_t channel = 0; channel < 3; channel++) {
        palette[2][channel] = static_cast<uint8_t>((2u * palette[0][channel] + palette[1][channel]) / 3u);
        palette[3][channel] = static_cast<uint8_t>((palette[0][channel] + 2u * palette[1][channel]) / 3u);
      }
      palette[2][3] = 255;
      palette[3][3] = 255;
    } else {
      for (uint32_t channel = 0; channel < 3; channel++) {
        palette[2][channel] = static_cast<uint8_t>((palette[0][channel] + palette[1][channel]) / 2u);
        palette[3][channel] = 0;
      }
      palette[2][3] = 255;
      palette[3][3] = 0;
    }
  }

  inline FingerprintColorSample sampleBc1FingerprintBlock(const uint8_t* block) {
    uint8_t palette[4][4] = {};
    decodeBc1FingerprintPalette(block, palette, false);

    uint32_t accumR = 0;
    uint32_t accumG = 0;
    uint32_t accumB = 0;
    uint32_t accumA = 0;
    uint32_t indices = static_cast<uint32_t>(block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24));

    for (uint32_t pixel = 0; pixel < 16; pixel++) {
      uint32_t paletteIndex = (indices >> (pixel * 2u)) & 0x3u;
      accumR += palette[paletteIndex][0];
      accumG += palette[paletteIndex][1];
      accumB += palette[paletteIndex][2];
      accumA += palette[paletteIndex][3];
    }

    FingerprintColorSample sample = {};
    sample.r = static_cast<uint8_t>(accumR / 16u);
    sample.g = static_cast<uint8_t>(accumG / 16u);
    sample.b = static_cast<uint8_t>(accumB / 16u);
    sample.a = static_cast<uint8_t>(accumA / 16u);
    return sample;
  }

  inline FingerprintColorSample sampleBc3FingerprintBlock(const uint8_t* block) {
    uint8_t alphaPalette[8] = {};
    alphaPalette[0] = block[0];
    alphaPalette[1] = block[1];

    if (alphaPalette[0] > alphaPalette[1]) {
      alphaPalette[2] = static_cast<uint8_t>((6u * alphaPalette[0] + alphaPalette[1]) / 7u);
      alphaPalette[3] = static_cast<uint8_t>((5u * alphaPalette[0] + 2u * alphaPalette[1]) / 7u);
      alphaPalette[4] = static_cast<uint8_t>((4u * alphaPalette[0] + 3u * alphaPalette[1]) / 7u);
      alphaPalette[5] = static_cast<uint8_t>((3u * alphaPalette[0] + 4u * alphaPalette[1]) / 7u);
      alphaPalette[6] = static_cast<uint8_t>((2u * alphaPalette[0] + 5u * alphaPalette[1]) / 7u);
      alphaPalette[7] = static_cast<uint8_t>((alphaPalette[0] + 6u * alphaPalette[1]) / 7u);
    } else {
      alphaPalette[2] = static_cast<uint8_t>((4u * alphaPalette[0] + alphaPalette[1]) / 5u);
      alphaPalette[3] = static_cast<uint8_t>((3u * alphaPalette[0] + 2u * alphaPalette[1]) / 5u);
      alphaPalette[4] = static_cast<uint8_t>((2u * alphaPalette[0] + 3u * alphaPalette[1]) / 5u);
      alphaPalette[5] = static_cast<uint8_t>((alphaPalette[0] + 4u * alphaPalette[1]) / 5u);
      alphaPalette[6] = 0;
      alphaPalette[7] = 255;
    }

    uint64_t alphaBits = 0;
    for (uint32_t i = 0; i < 6; i++)
      alphaBits |= static_cast<uint64_t>(block[2 + i]) << (8u * i);

    uint8_t palette[4][4] = {};
    decodeBc1FingerprintPalette(block + 8, palette, true);

    uint32_t accumR = 0;
    uint32_t accumG = 0;
    uint32_t accumB = 0;
    uint32_t accumA = 0;
    uint32_t colorIndices = static_cast<uint32_t>(block[12] | (block[13] << 8) | (block[14] << 16) | (block[15] << 24));

    for (uint32_t pixel = 0; pixel < 16; pixel++) {
      uint32_t colorIndex = (colorIndices >> (pixel * 2u)) & 0x3u;
      uint32_t alphaIndex = static_cast<uint32_t>((alphaBits >> (pixel * 3u)) & 0x7u);
      accumR += palette[colorIndex][0];
      accumG += palette[colorIndex][1];
      accumB += palette[colorIndex][2];
      accumA += alphaPalette[alphaIndex];
    }

    FingerprintColorSample sample = {};
    sample.r = static_cast<uint8_t>(accumR / 16u);
    sample.g = static_cast<uint8_t>(accumG / 16u);
    sample.b = static_cast<uint8_t>(accumB / 16u);
    sample.a = static_cast<uint8_t>(accumA / 16u);
    return sample;
  }

  inline FingerprintColorSample sampleInitialFingerprintBlock(DXGI_FORMAT format, const uint8_t* block) {
    return isBc3FingerprintFormat(format)
      ? sampleBc3FingerprintBlock(block)
      : sampleBc1FingerprintBlock(block);
  }

  inline void logCreateTexture2DInitialFingerprint(
      const D3D11_TEXTURE2D_DESC*   pDesc,
      const D3D11_SUBRESOURCE_DATA* pInitialData) {
    static uint32_t s_initialFingerprintCount = 0;

    if (!d3d11InfoLoggingEnabled())
      return;

    if (!pDesc || !pInitialData || !pInitialData[0].pSysMem)
      return;

    uint32_t mipLevels = pDesc->MipLevels ? pDesc->MipLevels : 1u;
    uint32_t arraySize = pDesc->ArraySize ? pDesc->ArraySize : 1u;
    if (!isTrackedInitialTextureFingerprintFormat(pDesc->Format)
     || mipLevels != 1u
     || arraySize != 1u
     || s_initialFingerprintCount >= 64u) {
      return;
    }

    s_initialFingerprintCount++;

    uint32_t blockBytes = isBc1FingerprintFormat(pDesc->Format) ? 8u : 16u;
    uint32_t blockCols = (pDesc->Width + 3u) / 4u;
    uint32_t blockRows = (pDesc->Height + 3u) / 4u;
    uint64_t rowSizeBytes = static_cast<uint64_t>(blockCols) * blockBytes;
    const uint8_t* base = static_cast<const uint8_t*>(pInitialData[0].pSysMem);

    uint64_t hash = 1469598103934665603ull;
    uint64_t zeroCount = 0;
    uint64_t ffCount = 0;
    for (uint32_t row = 0; row < blockRows; row++) {
      const uint8_t* rowPtr = base + row * pInitialData[0].SysMemPitch;
      for (uint64_t column = 0; column < rowSizeBytes; column++) {
        uint8_t value = rowPtr[column];
        hash ^= static_cast<uint64_t>(value);
        hash = fnv64Multiply(hash);
        zeroCount += value == 0u ? 1u : 0u;
        ffCount += value == 0xffu ? 1u : 0u;
      }
    }

    auto sampleAt = [&](uint32_t blockX, uint32_t blockY) {
      if (blockX >= blockCols)
        blockX = blockCols - 1u;
      if (blockY >= blockRows)
        blockY = blockRows - 1u;
      return sampleInitialFingerprintBlock(
        pDesc->Format,
        base + blockY * pInitialData[0].SysMemPitch + blockX * blockBytes);
    };

    FingerprintColorSample topLeft = sampleAt(0u, 0u);
    FingerprintColorSample center = sampleAt(blockCols / 2u, blockRows / 2u);
    FingerprintColorSample bottomRight = sampleAt(blockCols - 1u, blockRows - 1u);

    auto isNearWhite = [](const FingerprintColorSample& color) {
      return color.r >= 240u && color.g >= 240u && color.b >= 240u && color.a >= 240u;
    };

    auto isNearBlack = [](const FingerprintColorSample& color) {
      return color.r <= 15u && color.g <= 15u && color.b <= 15u && color.a <= 15u;
    };

    const char* classification = "sampled-varied";
    if (isNearWhite(topLeft) && isNearWhite(center) && isNearWhite(bottomRight))
      classification = "sampled-white";
    else if (isNearBlack(topLeft) && isNearBlack(center) && isNearBlack(bottomRight))
      classification = "sampled-black";

    logMessage(LogLevel::Info,
      "CreateTexture2D-initial-fingerprint: fmt=%u %ux%u pitch=%u bytes=%llu hash=0x%016llX zero=%llu ff=%llu class=%s tl=%u,%u,%u,%u mid=%u,%u,%u,%u br=%u,%u,%u,%u",
      static_cast<uint32_t>(pDesc->Format),
      pDesc->Width,
      pDesc->Height,
      pInitialData[0].SysMemPitch,
      static_cast<unsigned long long>(rowSizeBytes * blockRows),
      static_cast<unsigned long long>(hash),
      static_cast<unsigned long long>(zeroCount),
      static_cast<unsigned long long>(ffCount),
      classification,
      topLeft.r,
      topLeft.g,
      topLeft.b,
      topLeft.a,
      center.r,
      center.g,
      center.b,
      center.a,
      bottomRight.r,
      bottomRight.g,
      bottomRight.b,
      bottomRight.a);
  }

  inline UINT getWrappedResourceBindFlags(
      const D3D12_RESOURCE_DESC& resourceDesc,
      const D3D11_RESOURCE_FLAGS* resourceFlags) {
    if (resourceFlags)
      return resourceFlags->BindFlags;

    UINT bindFlags = 0;

    if (!(resourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
      bindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
      bindFlags |= D3D11_BIND_RENDER_TARGET;
    if (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
      bindFlags |= D3D11_BIND_DEPTH_STENCIL;
    if (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
      bindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    return bindFlags;
  }

  inline ImageViewDesc getDefaultImageViewDesc(const ImageDesc& imgDesc, ImageViewUsage usage) {
    ImageViewDesc viewDesc = {};
    uint32_t mipLevels = imgDesc.mipLevels ? imgDesc.mipLevels : 1u;

    viewDesc.format = imgDesc.format;
    viewDesc.usage = usage;
    viewDesc.mipLevelCount = (usage == ImageViewUsage::ShaderResource && imgDesc.sampleCount <= 1)
      ? mipLevels
      : 1u;
    viewDesc.arrayLayerCount = imgDesc.type == ImageType::Tex3D ? imgDesc.depth : imgDesc.arraySize;

    switch (imgDesc.type) {
      case ImageType::Tex1D:
        viewDesc.type = imgDesc.arraySize > 1 ? ImageViewType::View1DArray : ImageViewType::View1D;
        break;

      case ImageType::Tex3D:
        viewDesc.type = ImageViewType::View3D;
        break;

      case ImageType::Tex2D:
      default:
        if (usage == ImageViewUsage::ShaderResource && (imgDesc.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
          viewDesc.type = imgDesc.arraySize > 6 ? ImageViewType::ViewCubeArray : ImageViewType::ViewCube;
        } else if (imgDesc.sampleCount > 1) {
          viewDesc.type = imgDesc.arraySize > 1 ? ImageViewType::View2DMSArray : ImageViewType::View2DMS;
        } else if (imgDesc.arraySize > 1) {
          viewDesc.type = ImageViewType::View2DArray;
        } else {
          viewDesc.type = ImageViewType::View2D;
        }
        break;
    }

    return viewDesc;
  }

  inline bool initImageViewDescFromSrv(
      const D3D11_SHADER_RESOURCE_VIEW_DESC& srvDesc,
      const ImageDesc& imgDesc,
      ImageViewDesc& viewDesc) {
    viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::ShaderResource);
    viewDesc.format = resolveImageViewFormat(srvDesc.Format, imgDesc.format);

    switch (srvDesc.ViewDimension) {
      case D3D11_SRV_DIMENSION_TEXTURE1D:
        viewDesc.type = ImageViewType::View1D;
        viewDesc.baseMipLevel = srvDesc.Texture1D.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.Texture1D.MipLevels);
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        viewDesc.type = ImageViewType::View1DArray;
        viewDesc.baseMipLevel = srvDesc.Texture1DArray.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.Texture1DArray.MipLevels);
        viewDesc.baseArrayLayer = srvDesc.Texture1DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = srvDesc.Texture1DArray.ArraySize;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE2D:
        viewDesc.type = ImageViewType::View2D;
        viewDesc.baseMipLevel = srvDesc.Texture2D.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.Texture2D.MipLevels);
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        viewDesc.type = ImageViewType::View2DArray;
        viewDesc.baseMipLevel = srvDesc.Texture2DArray.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.Texture2DArray.MipLevels);
        viewDesc.baseArrayLayer = srvDesc.Texture2DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = srvDesc.Texture2DArray.ArraySize;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        viewDesc.type = ImageViewType::View2DMS;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        viewDesc.type = ImageViewType::View2DMSArray;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = srvDesc.Texture2DMSArray.FirstArraySlice;
        viewDesc.arrayLayerCount = srvDesc.Texture2DMSArray.ArraySize;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURE3D:
        viewDesc.type = ImageViewType::View3D;
        viewDesc.baseMipLevel = srvDesc.Texture3D.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.Texture3D.MipLevels);
        return true;

      case D3D11_SRV_DIMENSION_TEXTURECUBE:
        viewDesc.type = ImageViewType::ViewCube;
        viewDesc.baseMipLevel = srvDesc.TextureCube.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.TextureCube.MipLevels);
        viewDesc.arrayLayerCount = 6;
        return true;

      case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        viewDesc.type = ImageViewType::ViewCubeArray;
        viewDesc.baseMipLevel = srvDesc.TextureCubeArray.MostDetailedMip;
        viewDesc.mipLevelCount = resolveMipLevelCount(imgDesc.mipLevels, viewDesc.baseMipLevel, srvDesc.TextureCubeArray.MipLevels);
        viewDesc.baseArrayLayer = srvDesc.TextureCubeArray.First2DArrayFace;
        viewDesc.arrayLayerCount = srvDesc.TextureCubeArray.NumCubes * 6;
        return true;

      default:
        return false;
    }
  }

  inline bool initImageViewDescFromRtv(
      const D3D11_RENDER_TARGET_VIEW_DESC& rtvDesc,
      const ImageDesc& imgDesc,
      ImageViewDesc& viewDesc) {
    viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::RenderTarget);
    viewDesc.format = resolveImageViewFormat(rtvDesc.Format, imgDesc.format);

    switch (rtvDesc.ViewDimension) {
      case D3D11_RTV_DIMENSION_TEXTURE1D:
        viewDesc.type = ImageViewType::View1D;
        viewDesc.baseMipLevel = rtvDesc.Texture1D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        viewDesc.type = ImageViewType::View1DArray;
        viewDesc.baseMipLevel = rtvDesc.Texture1DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = rtvDesc.Texture1DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = rtvDesc.Texture1DArray.ArraySize;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE2D:
        viewDesc.type = ImageViewType::View2D;
        viewDesc.baseMipLevel = rtvDesc.Texture2D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        viewDesc.type = ImageViewType::View2DArray;
        viewDesc.baseMipLevel = rtvDesc.Texture2DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = rtvDesc.Texture2DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = rtvDesc.Texture2DArray.ArraySize;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        viewDesc.type = ImageViewType::View2DMS;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        viewDesc.type = ImageViewType::View2DMSArray;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = rtvDesc.Texture2DMSArray.FirstArraySlice;
        viewDesc.arrayLayerCount = rtvDesc.Texture2DMSArray.ArraySize;
        return true;

      case D3D11_RTV_DIMENSION_TEXTURE3D:
        viewDesc.type = ImageViewType::View3D;
        viewDesc.baseMipLevel = rtvDesc.Texture3D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = rtvDesc.Texture3D.FirstWSlice;
        viewDesc.arrayLayerCount = rtvDesc.Texture3D.WSize;
        return true;

      default:
        return false;
    }
  }

  inline bool initImageViewDescFromDsv(
      const D3D11_DEPTH_STENCIL_VIEW_DESC& dsvDesc,
      const ImageDesc& imgDesc,
      ImageViewDesc& viewDesc) {
    viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::DepthStencil);
    viewDesc.format = resolveDepthStencilFormat(resolveImageViewFormat(dsvDesc.Format, imgDesc.format));
    if (dsvDesc.Flags & D3D11_DSV_READ_ONLY_DEPTH)
      viewDesc.flags = viewDesc.flags | ImageViewFlags::ReadOnlyDepth;
    if (dsvDesc.Flags & D3D11_DSV_READ_ONLY_STENCIL)
      viewDesc.flags = viewDesc.flags | ImageViewFlags::ReadOnlyStencil;

    switch (dsvDesc.ViewDimension) {
      case D3D11_DSV_DIMENSION_TEXTURE1D:
        viewDesc.type = ImageViewType::View1D;
        viewDesc.baseMipLevel = dsvDesc.Texture1D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        viewDesc.type = ImageViewType::View1DArray;
        viewDesc.baseMipLevel = dsvDesc.Texture1DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = dsvDesc.Texture1DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = dsvDesc.Texture1DArray.ArraySize;
        return true;

      case D3D11_DSV_DIMENSION_TEXTURE2D:
        viewDesc.type = ImageViewType::View2D;
        viewDesc.baseMipLevel = dsvDesc.Texture2D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        viewDesc.type = ImageViewType::View2DArray;
        viewDesc.baseMipLevel = dsvDesc.Texture2DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = dsvDesc.Texture2DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = dsvDesc.Texture2DArray.ArraySize;
        return true;

      case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        viewDesc.type = ImageViewType::View2DMS;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        viewDesc.type = ImageViewType::View2DMSArray;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = dsvDesc.Texture2DMSArray.FirstArraySlice;
        viewDesc.arrayLayerCount = dsvDesc.Texture2DMSArray.ArraySize;
        return true;

      default:
        return false;
    }
  }

  inline bool initImageViewDescFromUav(
      const D3D11_UNORDERED_ACCESS_VIEW_DESC& uavDesc,
      const ImageDesc& imgDesc,
      ImageViewDesc& viewDesc) {
    viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::UnorderedAccess);
    viewDesc.format = resolveImageViewFormat(uavDesc.Format, imgDesc.format);

    switch (uavDesc.ViewDimension) {
      case D3D11_UAV_DIMENSION_TEXTURE1D:
        viewDesc.type = ImageViewType::View1D;
        viewDesc.baseMipLevel = uavDesc.Texture1D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        viewDesc.type = ImageViewType::View1DArray;
        viewDesc.baseMipLevel = uavDesc.Texture1DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = uavDesc.Texture1DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = uavDesc.Texture1DArray.ArraySize;
        return true;

      case D3D11_UAV_DIMENSION_TEXTURE2D:
        viewDesc.type = ImageViewType::View2D;
        viewDesc.baseMipLevel = uavDesc.Texture2D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.arrayLayerCount = 1;
        return true;

      case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        viewDesc.type = ImageViewType::View2DArray;
        viewDesc.baseMipLevel = uavDesc.Texture2DArray.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = uavDesc.Texture2DArray.FirstArraySlice;
        viewDesc.arrayLayerCount = uavDesc.Texture2DArray.ArraySize;
        return true;

      case D3D11_UAV_DIMENSION_TEXTURE3D:
        viewDesc.type = ImageViewType::View3D;
        viewDesc.baseMipLevel = uavDesc.Texture3D.MipSlice;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = uavDesc.Texture3D.FirstWSlice;
        viewDesc.arrayLayerCount = uavDesc.Texture3D.WSize;
        return true;

      default:
        return false;
    }
  }

  inline uint64_t getBufferViewElementSize(
      const D3D11_BUFFER_DESC& bufferDesc,
      const BufferViewDesc& viewDesc) {
    if (viewDesc.raw)
      return sizeof(uint32_t);

    if (viewDesc.format == DXGI_FORMAT_UNKNOWN)
      return bufferDesc.StructureByteStride;

    return getFormatBpp(viewDesc.format);
  }

  inline bool finalizeBufferViewDesc(
      const D3D11_BUFFER_DESC& bufferDesc,
      BufferViewDesc& viewDesc) {
    uint64_t elementSize = getBufferViewElementSize(bufferDesc, viewDesc);
    if (!elementSize)
      return false;

    uint64_t totalElements = bufferDesc.ByteWidth / elementSize;
    if (viewDesc.offset > totalElements)
      return false;

    if (viewDesc.size == 0)
      viewDesc.size = totalElements - viewDesc.offset;

    return viewDesc.size != 0 && (viewDesc.offset + viewDesc.size) <= totalElements;
  }

  inline bool initDefaultBufferViewDesc(
      const D3D11_BUFFER_DESC& bufferDesc,
      BufferViewDesc& viewDesc) {
    viewDesc = {};

    if (bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) {
      viewDesc.format = DXGI_FORMAT_R32_TYPELESS;
      viewDesc.raw = true;
      return finalizeBufferViewDesc(bufferDesc, viewDesc);
    }

    if (bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
      viewDesc.format = DXGI_FORMAT_UNKNOWN;
      return finalizeBufferViewDesc(bufferDesc, viewDesc);
    }

    return false;
  }

  inline bool initBufferViewDescFromSrv(
      const D3D11_SHADER_RESOURCE_VIEW_DESC& srvDesc,
      const D3D11_BUFFER_DESC& bufferDesc,
      BufferViewDesc& viewDesc) {
    viewDesc = {};

    switch (srvDesc.ViewDimension) {
      case D3D11_SRV_DIMENSION_BUFFER:
        viewDesc.format = srvDesc.Format;
        viewDesc.offset = srvDesc.Buffer.FirstElement;
        viewDesc.size = srvDesc.Buffer.NumElements;
        break;

      case D3D11_SRV_DIMENSION_BUFFEREX:
        viewDesc.format = srvDesc.Format;
        viewDesc.offset = srvDesc.BufferEx.FirstElement;
        viewDesc.size = srvDesc.BufferEx.NumElements;
        viewDesc.raw = (srvDesc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
        if (viewDesc.raw)
          viewDesc.format = DXGI_FORMAT_R32_TYPELESS;
        break;

      default:
        return false;
    }

    return finalizeBufferViewDesc(bufferDesc, viewDesc);
  }

  inline bool initBufferViewDescFromUav(
      const D3D11_UNORDERED_ACCESS_VIEW_DESC& uavDesc,
      const D3D11_BUFFER_DESC& bufferDesc,
      BufferViewDesc& viewDesc) {
    viewDesc = {};

    if (uavDesc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER)
      return false;

    viewDesc.format = uavDesc.Format;
    viewDesc.offset = uavDesc.Buffer.FirstElement;
    viewDesc.size = uavDesc.Buffer.NumElements;
    viewDesc.raw = (uavDesc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) != 0;
    viewDesc.hasCounter = (uavDesc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) != 0;
    if (viewDesc.raw)
      viewDesc.format = DXGI_FORMAT_R32_TYPELESS;

    return finalizeBufferViewDesc(bufferDesc, viewDesc);
  }

  // ============================================================
  // COM Helper: Base class for D3D11 device children
  // ============================================================

  // Follows native D3D11 lifetime rules. Public references belong to the
  // application and keep the device alive. Private references are taken by
  // the runtime itself (context bindings, a view's resource): they keep this
  // object alive but not the device, so bound state cannot leak a device.
  // All public references together own one private reference.
  template<typename Base>
  class D3D11DeviceChild : public Base {
  public:
    // Defined after D3D11Device, since the initial public reference also
    // takes a device reference.
    D3D11DeviceChild(D3D11Device* pDevice);

    virtual ~D3D11DeviceChild() = default;

    ULONG STDMETHODCALLTYPE AddRef() final {
      ULONG ref = ++m_refCount;
      if (ref == 1) {
        AddRefPrivate();
        acquireDevice(m_device);
      }
      return ref;
    }

    ULONG STDMETHODCALLTYPE Release() final {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting()) {
        // Destroy this object before the device it may still refer to.
        D3D11Device* device = m_device;
        ReleasePrivate();
        releaseDevice(device);
      }
      return ref;
    }

    void AddRefPrivate() {
      ++m_refPrivate;
    }

    void ReleasePrivate() {
      if (--m_refPrivate == 0)
        delete this;
    }

    // Starts loading the cache line binding changes, so that the lines of
    // several objects bound at once arrive together.
    void prefetchForBind() const {
      _m_prefetchw(const_cast<std::atomic<ULONG>*>(&m_refCount));
    }

    // A public reference to an object found in a cache, unless the object
    // is already being destroyed.
    bool tryAddRefFromCache() {
      ULONG refPrivate = m_refPrivate.load();
      do {
        if (!refPrivate)
          return false;
      } while (!m_refPrivate.compare_exchange_weak(refPrivate, refPrivate + 1));
      AddRef();
      ReleasePrivate();
      return true;
    }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) final;

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID guid, UINT* pDataSize, void* pData) final {
      return m_privateData.getData(guid, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID guid, UINT DataSize, const void* pData) final {
      return m_privateData.setData(guid, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID guid, const IUnknown* pData) final {
      return m_privateData.setInterface(guid, pData);
    }

  protected:
    static void acquireDevice(D3D11Device* device);
    static void releaseDevice(D3D11Device* device);

    // Contexts on several threads change these on every bind: a cache line
    // of their own keeps that from evicting what is read with them.
    alignas(64) std::atomic<ULONG> m_refCount;
    std::atomic<ULONG> m_refPrivate;
    // What binding reads (the backend object, a view's resource), on the
    // cache line binding changes anyway: one cache miss per bound object.
    std::atomic<void*> m_bindData[2] = {};
    void setBindData(void* first, void* second) {
      m_bindData[0].store(first, std::memory_order_relaxed);
      m_bindData[1].store(second, std::memory_order_relaxed);
    }
    void* bindData(int index) const { return m_bindData[index].load(std::memory_order_relaxed); }
    alignas(64) D3D11Device* m_device;
    ComPrivateData     m_privateData;
  };

  // ============================================================
  // D3D11DxgiResource
  // ============================================================

  // IDXGIResource1 and IDXGISurface2 of a D3D11 resource. Embedded in the
  // resource, whose IUnknown it shares, or created for one subresource by
  // CreateSubresourceSurface as an object of its own that keeps the
  // resource alive. Private data is the resource's, as in D3D11. Shared
  // resources with a keyed mutex also answer IDXGIKeyedMutex.
  class D3D11DxgiResource final : public IDXGIResource1, public IDXGISurface2, public IDXGIKeyedMutex {
  public:
    D3D11DxgiResource(ID3D11Resource* resource, D3D11Device* device, UINT subresource, bool separate)
      : m_resource(resource), m_device(device), m_subresource(subresource), m_separate(separate) {
      if (m_separate)
        m_resource->AddRef();
    }

    ~D3D11DxgiResource() {
      releaseDC();
      releaseShared();
      if (m_separate)
        m_resource->Release();
    }

    D3D11DxgiResource(const D3D11DxgiResource&) = delete;
    D3D11DxgiResource& operator=(const D3D11DxgiResource&) = delete;

    // Answers the DXGI interfaces for the owning resource; surfaces only for
    // 2D textures with one subresource.
    bool queryInterface(REFIID riid, void** ppvObject, bool surface) {
      if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject)
       || riid == __uuidof(IDXGIResource) || riid == __uuidof(IDXGIResource1)) {
        *ppvObject = static_cast<IDXGIResource1*>(this);
        AddRef();
        return true;
      }
      if (surface && (riid == __uuidof(IDXGISurface) || riid == __uuidof(IDXGISurface1)
                   || riid == __uuidof(IDXGISurface2))) {
        *ppvObject = static_cast<IDXGISurface2*>(this);
        AddRef();
        return true;
      }
      if (riid == __uuidof(IDXGIKeyedMutex) && m_shared && m_shared->hasKeyedMutex()) {
        *ppvObject = static_cast<IDXGIKeyedMutex*>(this);
        AddRef();
        return true;
      }
      return false;
    }

    // Set on resources shared with other devices.
    const std::shared_ptr<D3D11SharedResource>& shared() const { return m_shared; }
    void setShared(std::shared_ptr<D3D11SharedResource> shared);

    UINT evictionPriority() const { return m_evictionPriority.load(std::memory_order_relaxed); }
    void setEvictionPriority(UINT priority) { m_evictionPriority.store(priority, std::memory_order_relaxed); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject)
        return E_POINTER;
      *ppvObject = nullptr;
      if (m_separate) {
        if (riid == __uuidof(IUnknown)) {
          *ppvObject = static_cast<IDXGISurface2*>(this);
          AddRef();
          return S_OK;
        }
        if (queryInterface(riid, ppvObject, true))
          return S_OK;
      }
      return m_resource->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
      if (!m_separate)
        return m_resource->AddRef();
      return ++m_refCount;
    }
    ULONG STDMETHODCALLTYPE Release() override {
      if (!m_separate)
        return m_resource->Release();
      const ULONG ref = --m_refCount;
      if (!ref && !processExiting())
        delete this;
      return ref;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
      return m_resource->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
      return m_resource->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
      return m_resource->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // IDXGIResource
    HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE* pUsage) override;
    HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override {
      setEvictionPriority(priority);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT* pPriority) override {
      if (!pPriority)
        return E_INVALIDARG;
      *pPriority = evictionPriority();
      return S_OK;
    }

    // IDXGIResource1
    HRESULT STDMETHODCALLTYPE CreateSubresourceSurface(UINT index, IDXGISurface2** ppSurface) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES* pAttributes, DWORD access,
                                                 LPCWSTR name, HANDLE* pHandle) override;

    // IDXGIKeyedMutex
    HRESULT STDMETHODCALLTYPE AcquireSync(UINT64 key, DWORD milliseconds) override;
    HRESULT STDMETHODCALLTYPE ReleaseSync(UINT64 key) override;

    // IDXGISurface
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT* pLockedRect, UINT flags) override;
    HRESULT STDMETHODCALLTYPE Unmap() override;

    // IDXGISurface1
    HRESULT STDMETHODCALLTYPE GetDC(BOOL discard, HDC* phdc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(RECT* pDirtyRect) override;

    // IDXGISurface2
    HRESULT STDMETHODCALLTYPE GetResource(REFIID riid, void** ppParentResource, UINT* pSubresourceIndex) override {
      if (pSubresourceIndex)
        *pSubresourceIndex = m_subresource;
      return m_resource->QueryInterface(riid, ppParentResource);
    }

  private:
    bool subresourceExtent(D3D11_TEXTURE2D_DESC& desc, UINT& width, UINT& height) const;
    void releaseDC();
    void releaseShared();

    ID3D11Resource*     m_resource;
    D3D11Device*        m_device;
    UINT                m_subresource;
    bool                m_separate;
    std::atomic<ULONG>  m_refCount{1};
    std::atomic<UINT>   m_evictionPriority{DXGI_RESOURCE_PRIORITY_NORMAL};
    // GetDC state: a DIB section the application draws into.
    HDC                 m_dc = nullptr;
    HBITMAP             m_bitmap = nullptr;
    HGDIOBJ             m_previousBitmap = nullptr;
    void*               m_bits = nullptr;
    std::shared_ptr<D3D11SharedResource> m_shared;
  };

  // ============================================================
  // D3D11Buffer
  // ============================================================

  class D3D11Buffer : public D3D11DeviceChild<ID3D11Buffer> {
  public:
    D3D11Buffer(D3D11Device* pDevice,
                const D3D11_BUFFER_DESC& desc,
                Rc<IBackendBuffer> buffer,
                const D3D11_SUBRESOURCE_DATA* initialData = nullptr)
      : D3D11DeviceChild<ID3D11Buffer>(pDevice)
      , m_dxgi(this, pDevice, 0, false)
      , m_desc(desc)
      , m_buffer(std::move(buffer))
      , m_deferredAllocation(!m_buffer) {
      setBindData(m_buffer.get(), nullptr);
      uint64_t shadowDataSize = getTrackedBufferShadowSize(m_desc);
      if (shadowDataSize) {
        m_shadowData.resize(static_cast<size_t>(shadowDataSize));

        if (initialData && initialData->pSysMem) {
          size_t copySize = static_cast<size_t>(shadowDataSize);
          std::memcpy(m_shadowData.data(), initialData->pSysMem, copySize);
        } else {
          std::memset(m_shadowData.data(), 0, m_shadowData.size());
        }
      }
    }

    ~D3D11Buffer() override = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Resource)
       || riid == __uuidof(ID3D11Buffer)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      // Direct3D 10 (see d3d10_interfaces.h).
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10Resource)
       || riid == __uuidof(ID3D10Buffer)) {
        *ppvObject = static_cast<ID3D10Buffer*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      if (m_dxgi.queryInterface(riid, ppvObject, true))
        return S_OK;
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pDimension) override {
      *pDimension = D3D11_RESOURCE_DIMENSION_BUFFER;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {
      m_dxgi.setEvictionPriority(EvictionPriority);
    }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return m_dxgi.evictionPriority(); }

    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    IBackendBuffer* GetBackendBuffer() const {
      if (void* buffer = bindData(0))
        return static_cast<IBackendBuffer*>(buffer);
      if (!m_deferredAllocation)
        return nullptr;
      std::call_once(m_allocationOnce, [this] { allocateDeferredBuffer(); });
      return m_buffer.get();
    }

    const D3D11_BUFFER_DESC& GetDescRef() const { return m_desc; }
    D3D11DxgiResource& dxgiResource() { return m_dxgi; }

    // The views follow on the context, in order with its commands.
    Rc<IBackendBuffer> ReplaceBackendBuffer(Rc<IBackendBuffer> buffer, IBackendContext* context) {
      Rc<IBackendBuffer> oldBuffer = std::move(m_buffer);
      m_buffer = std::move(buffer);
      setBindData(m_buffer.get(), nullptr);
      std::vector<IBackendBufferView*> views;
      if (m_backendViewCount.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(m_viewMutex);
        views = m_backendViews;
      }
      if (!views.empty() && context)
        context->retargetBufferViews(m_buffer.get(), views);
      return oldBuffer;
    }

    void RegisterBackendBufferView(IBackendBufferView* view) {
      if (!view)
        return;

      std::lock_guard<std::mutex> lock(m_viewMutex);
      auto it = std::find(m_backendViews.begin(), m_backendViews.end(), view);
      if (it == m_backendViews.end()) {
        m_backendViews.push_back(view);
        m_backendViewCount.store(uint32_t(m_backendViews.size()), std::memory_order_release);
      }
    }

    void UnregisterBackendBufferView(IBackendBufferView* view) {
      if (!view)
        return;

      std::lock_guard<std::mutex> lock(m_viewMutex);
      auto it = std::find(m_backendViews.begin(), m_backendViews.end(), view);
      if (it != m_backendViews.end()) {
        m_backendViews.erase(it);
        m_backendViewCount.store(uint32_t(m_backendViews.size()), std::memory_order_release);
      }
    }

    bool HasShadowData() const { return !m_shadowData.empty(); }

    uint64_t GetShadowSize() const { return m_shadowData.size(); }

    const uint8_t* GetShadowData() const {
      return m_shadowData.empty() ? nullptr : m_shadowData.data();
    }

    void UpdateShadowData(uint64_t offset, const void* data, uint64_t size) {
      if (!data || m_shadowData.empty() || offset >= m_shadowData.size())
        return;

      size_t copyOffset = static_cast<size_t>(offset);
      size_t copySize = static_cast<size_t>(size);
      if (copySize > m_shadowData.size() - copyOffset)
        copySize = m_shadowData.size() - copyOffset;

      std::memcpy(m_shadowData.data() + copyOffset, data, copySize);
    }

    void CopyShadowDataFrom(const D3D11Buffer* src, uint64_t dstOffset, uint64_t srcOffset, uint64_t size) {
      if (!src || m_shadowData.empty() || src->m_shadowData.empty())
        return;
      if (dstOffset >= m_shadowData.size() || srcOffset >= src->m_shadowData.size())
        return;

      size_t dstCopyOffset = static_cast<size_t>(dstOffset);
      size_t srcCopyOffset = static_cast<size_t>(srcOffset);
      size_t copySize = static_cast<size_t>(size);

      if (copySize > m_shadowData.size() - dstCopyOffset)
        copySize = m_shadowData.size() - dstCopyOffset;
      if (copySize > src->m_shadowData.size() - srcCopyOffset)
        copySize = src->m_shadowData.size() - srcCopyOffset;
      if (!copySize)
        return;

      std::memmove(m_shadowData.data() + dstCopyOffset, src->m_shadowData.data() + srcCopyOffset, copySize);
    }

  private:
    D3D11DxgiResource    m_dxgi;
    // Defined after D3D11Device, which owns the backend.
    void allocateDeferredBuffer() const;

    D3D11_BUFFER_DESC  m_desc;
    Rc<IBackendBuffer> m_buffer;
    const bool         m_deferredAllocation;
    mutable std::once_flag m_allocationOnce;
    std::vector<uint8_t> m_shadowData;
    std::mutex m_viewMutex;
    std::vector<IBackendBufferView*> m_backendViews;
    std::atomic<uint32_t> m_backendViewCount{0};

    D3D10Buffer m_d3d10{this};
  };

  // Memory a deferred context gave a buffer with WRITE_DISCARD: running the
  // command list makes it the buffer's contents again, as D3D11 replays the
  // discard there.
  class RecordedDiscard {
  public:
    RecordedDiscard(D3D11Buffer* buffer, Rc<IBackendBuffer> contents)
    : m_buffer(buffer), m_contents(std::move(contents)) {
      m_buffer->AddRefPrivate();
    }
    RecordedDiscard(RecordedDiscard&& other) noexcept
    : m_buffer(other.m_buffer), m_contents(std::move(other.m_contents)) {
      other.m_buffer = nullptr;
    }
    RecordedDiscard& operator=(RecordedDiscard&& other) noexcept {
      if (this != &other) {
        if (m_buffer)
          m_buffer->ReleasePrivate();
        m_buffer = other.m_buffer;
        m_contents = std::move(other.m_contents);
        other.m_buffer = nullptr;
      }
      return *this;
    }
    RecordedDiscard(const RecordedDiscard&) = delete;
    RecordedDiscard& operator=(const RecordedDiscard&) = delete;
    ~RecordedDiscard() {
      if (m_buffer)
        m_buffer->ReleasePrivate();
    }

    D3D11Buffer* buffer() const { return m_buffer; }
    const Rc<IBackendBuffer>& contents() const { return m_contents; }

  private:
    D3D11Buffer*       m_buffer;
    Rc<IBackendBuffer> m_contents;
  };

  // How a command reaches a texture with a forced multisampled copy outside
  // the copy: it reads the image, writes part of it, or replaces all of it
  // (full clears, whole copies and updates).
  enum class MsaaUse : uint8_t { Read, Write, Overwrite };

  // A texture with a forced multisampled copy that a deferred context read or
  // wrote: running the command list brings the texture's image up to date
  // first, unless the list starts by replacing it, and after writes makes
  // it the newer of the two.
  class D3D11Texture2D;

  class RecordedMsaaUse {
  public:
    RecordedMsaaUse(D3D11Texture2D* texture, MsaaUse first);
    RecordedMsaaUse(RecordedMsaaUse&& other) noexcept
    : m_texture(other.m_texture), m_first(other.m_first), m_write(other.m_write) {
      other.m_texture = nullptr;
    }
    RecordedMsaaUse& operator=(RecordedMsaaUse&& other) noexcept;
    RecordedMsaaUse(const RecordedMsaaUse&) = delete;
    RecordedMsaaUse& operator=(const RecordedMsaaUse&) = delete;
    ~RecordedMsaaUse();

    D3D11Texture2D* texture() const { return m_texture; }
    MsaaUse first() const { return m_first; }
    bool write() const { return m_write; }
    void markWrite() { m_write = true; }

  private:
    D3D11Texture2D* m_texture;
    MsaaUse         m_first;
    bool            m_write;
  };

  // ============================================================
  // D3D11Texture1D
  // ============================================================

  class D3D11Texture1D : public D3D11DeviceChild<ID3D11Texture1D> {
  public:
    D3D11Texture1D(D3D11Device* pDevice,
                   const D3D11_TEXTURE1D_DESC& desc,
                   Rc<IBackendImage> image)
      : D3D11DeviceChild<ID3D11Texture1D>(pDevice)
      , m_dxgi(this, pDevice, 0, false)
      , m_desc(desc)
      , m_image(std::move(image)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Resource)
       || riid == __uuidof(ID3D11Texture1D)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10Resource)
       || riid == __uuidof(ID3D10Texture1D)) {
        *ppvObject = static_cast<ID3D10Texture1D*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      if (m_dxgi.queryInterface(riid, ppvObject, m_desc.MipLevels == 1 && m_desc.ArraySize == 1))
        return S_OK;
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pDimension) override {
      *pDimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {
      m_dxgi.setEvictionPriority(EvictionPriority);
    }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return m_dxgi.evictionPriority(); }

    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    IBackendImage* GetBackendImage() const { return m_image.get(); }
    D3D11DxgiResource& dxgiResource() { return m_dxgi; }

  private:
    D3D11DxgiResource    m_dxgi;
    D3D11_TEXTURE1D_DESC m_desc;
    Rc<IBackendImage>    m_image;

    D3D10Texture1D m_d3d10{this};
  };

  // ============================================================
  // D3D11Texture2D
  // ============================================================

  class D3D11Texture2D : public D3D11DeviceChild<ID3D11Texture2D1> {
  public:
    D3D11Texture2D(D3D11Device* pDevice,
                   const D3D11_TEXTURE2D_DESC& desc,
                   Rc<IBackendImage> image)
      : D3D11DeviceChild<ID3D11Texture2D1>(pDevice)
      , m_dxgi(this, pDevice, 0, false)
      , m_desc(desc)
      , m_image(std::move(image)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Resource)
       || riid == __uuidof(ID3D11Texture2D)
       || riid == __uuidof(ID3D11Texture2D1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10Resource)
       || riid == __uuidof(ID3D10Texture2D)) {
        *ppvObject = static_cast<ID3D10Texture2D*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      if (m_dxgi.queryInterface(riid, ppvObject, m_desc.MipLevels == 1 && m_desc.ArraySize == 1))
        return S_OK;
      logMessage(LogLevel::Debug, "D3D11Texture2D::QueryInterface unsupported riid=%s",
        formatGuid(riid).c_str());
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pDimension) override {
      *pDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {
      m_dxgi.setEvictionPriority(EvictionPriority);
    }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return m_dxgi.evictionPriority(); }

    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_TEXTURE2D_DESC1* pDesc) override {
      pDesc->Width          = m_desc.Width;
      pDesc->Height         = m_desc.Height;
      pDesc->MipLevels      = m_desc.MipLevels;
      pDesc->ArraySize      = m_desc.ArraySize;
      pDesc->Format         = m_desc.Format;
      pDesc->SampleDesc     = m_desc.SampleDesc;
      pDesc->Usage          = m_desc.Usage;
      pDesc->BindFlags      = m_desc.BindFlags;
      pDesc->CPUAccessFlags = m_desc.CPUAccessFlags;
      pDesc->MiscFlags      = m_desc.MiscFlags;
      pDesc->TextureLayout  = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    }

    IBackendImage* GetBackendImage() const { return m_image.get(); }
    D3D11DxgiResource& dxgiResource() { return m_dxgi; }

    // Forced multisampling (dxup.conf d3d11.msaaForce): render target and
    // depth views draw into a multisampled copy of the texture; everything
    // else uses its image. Which of the two is newer, as the immediate
    // context sees it in the order of its commands, and whether the copy
    // holds multisampled contents: drawn into since the texture was last
    // cleared, replaced or discarded as a whole.
    enum class MsaaState : uint8_t { Synced, ShadowNewer, ImageNewer };

    MsaaState GetMsaaState() const { return m_msaaState; }
    bool HasMsaaContent() const { return m_msaaContent; }
    void SetMsaaState(MsaaState state) {
      m_msaaState = state;
      m_msaaImageCopied = false;
      if (state == MsaaState::ShadowNewer)
        m_msaaContent = true;
    }
    // The contents were cleared, replaced or discarded as a whole.
    void ResetMsaaState(MsaaState state) {
      m_msaaState = state;
      m_msaaImageCopied = false;
      m_msaaContent = false;
      m_msaaImageStencilOld = false;
      m_msaaShadowStencilOld = false;
    }
    // Depth-stencil textures: resolving and refilling the copy carries depth
    // only. Whether the image's or the copy's stencil is older than the
    // other's, brought up to date when something uses it.
    bool IsMsaaImageStencilOld() const { return m_msaaImageStencilOld; }
    bool IsMsaaShadowStencilOld() const { return m_msaaShadowStencilOld; }
    void SetMsaaImageStencilOld(bool old) { m_msaaImageStencilOld = old; }
    void SetMsaaShadowStencilOld(bool old) { m_msaaShadowStencilOld = old; }
    // Copies of whole textures: the image was replaced by another texture's
    // (the copy is older now). When the copy then has to be refilled from
    // the image because it is drawn into, later copies into this texture
    // take the other texture's copy along: one copy instead of a refill
    // pass, with every sample's depth and the stencil kept.
    void MarkMsaaImageCopied() { m_msaaImageCopied = true; }
    void MarkMsaaRefill() {
      if (m_msaaImageCopied)
        m_msaaCopyShadow = true;
    }
    bool CopiesMsaaShadow() const { return m_msaaCopyShadow; }
    // Image and copy were copied from a texture whose two were the same.
    void CopyMsaaStateFrom(const D3D11Texture2D& source) {
      m_msaaState = MsaaState::Synced;
      m_msaaImageCopied = false;
      m_msaaContent = source.m_msaaContent;
      m_msaaImageStencilOld = source.m_msaaImageStencilOld;
      m_msaaShadowStencilOld = source.m_msaaShadowStencilOld;
    }
    // Depth-tested geometry was drawn into the copy at some point: the
    // texture keeps being drawn multisampled (msaaScope geometry), so that
    // draws before the depth-tested ones each frame need no refill.
    void MarkMsaaGeometry() { m_msaaGeometry = true; }
    bool IsMsaaGeometry() const { return m_msaaGeometry; }

    void SetMsaaShadow(Rc<IBackendImage> shadow, const ForcedMsaaFormats& formats) {
      m_msaaShadow = std::move(shadow);
      m_msaaFormats = formats;
    }
    IBackendImage* GetMsaaShadow() const { return m_msaaShadow.get(); }
    const ForcedMsaaFormats& GetMsaaFormats() const { return m_msaaFormats; }
    // Render target views name the format the application draws in, which
    // resolving and refilling must use.
    void SetMsaaColorFormat(DXGI_FORMAT format) { m_msaaFormats.typed = format; }

  private:
    D3D11DxgiResource    m_dxgi;
    D3D11_TEXTURE2D_DESC m_desc;
    Rc<IBackendImage>    m_image;
    Rc<IBackendImage>    m_msaaShadow;
    ForcedMsaaFormats    m_msaaFormats;
    MsaaState            m_msaaState = MsaaState::Synced;
    bool                 m_msaaContent = false;
    bool                 m_msaaGeometry = false;
    bool                 m_msaaImageCopied = false;
    bool                 m_msaaCopyShadow = false;
    bool                 m_msaaImageStencilOld = false;
    bool                 m_msaaShadowStencilOld = false;

    D3D10Texture2D m_d3d10{this};
  };

  // ============================================================
  // D3D11Texture3D
  // ============================================================

  class D3D11Texture3D : public D3D11DeviceChild<ID3D11Texture3D1> {
  public:
    D3D11Texture3D(D3D11Device* pDevice,
                   const D3D11_TEXTURE3D_DESC& desc,
                   Rc<IBackendImage> image)
      : D3D11DeviceChild<ID3D11Texture3D1>(pDevice)
      , m_dxgi(this, pDevice, 0, false)
      , m_desc(desc)
      , m_image(std::move(image)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Resource)
       || riid == __uuidof(ID3D11Texture3D)
       || riid == __uuidof(ID3D11Texture3D1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10Resource)
       || riid == __uuidof(ID3D10Texture3D)) {
        *ppvObject = static_cast<ID3D10Texture3D*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      if (m_dxgi.queryInterface(riid, ppvObject, false))
        return S_OK;
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pDimension) override {
      *pDimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {
      m_dxgi.setEvictionPriority(EvictionPriority);
    }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return m_dxgi.evictionPriority(); }

    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_TEXTURE3D_DESC1* pDesc) override {
      pDesc->Width          = m_desc.Width;
      pDesc->Height         = m_desc.Height;
      pDesc->Depth          = m_desc.Depth;
      pDesc->MipLevels      = m_desc.MipLevels;
      pDesc->Format         = m_desc.Format;
      pDesc->Usage          = m_desc.Usage;
      pDesc->BindFlags      = m_desc.BindFlags;
      pDesc->CPUAccessFlags = m_desc.CPUAccessFlags;
      pDesc->MiscFlags      = m_desc.MiscFlags;
      pDesc->TextureLayout  = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    }

    IBackendImage* GetBackendImage() const { return m_image.get(); }
    D3D11DxgiResource& dxgiResource() { return m_dxgi; }

  private:
    D3D11DxgiResource    m_dxgi;
    D3D11_TEXTURE3D_DESC m_desc;
    Rc<IBackendImage>    m_image;

    D3D10Texture3D m_d3d10{this};
  };

  // ============================================================
  // Private resource references
  // ============================================================

  // A view keeps its resource alive with a private reference, as the native
  // runtime does, so a view that is only referenced by a context binding
  // does not keep the device alive through its resource.
  template<typename Fn>
  bool visitResourceImpl(ID3D11Resource* resource, Fn&& fn) {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    switch (dimension) {
      case D3D11_RESOURCE_DIMENSION_BUFFER:
        fn(static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(resource)));
        return true;
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        fn(static_cast<D3D11Texture1D*>(static_cast<ID3D11Texture1D*>(resource)));
        return true;
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        fn(static_cast<D3D11Texture2D*>(static_cast<ID3D11Texture2D*>(resource)));
        return true;
      case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        fn(static_cast<D3D11Texture3D*>(static_cast<ID3D11Texture3D*>(resource)));
        return true;
      default:
        return false;
    }
  }

  inline void addResourcePrivateRef(ID3D11Resource* resource) {
    if (resource && !visitResourceImpl(resource, [](auto* impl) { impl->AddRefPrivate(); }))
      resource->AddRef();
  }

  inline void releaseResourcePrivateRef(ID3D11Resource* resource) {
    if (resource && !visitResourceImpl(resource, [](auto* impl) { impl->ReleasePrivate(); }))
      resource->Release();
  }

  // ============================================================
  // D3D11ShaderResourceView
  // ============================================================

  class D3D11ShaderResourceView : public D3D11DeviceChild<ID3D11ShaderResourceView1> {
  public:
    D3D11ShaderResourceView(D3D11Device* pDevice,
                            const D3D11_SHADER_RESOURCE_VIEW_DESC& desc,
                            ID3D11Resource* pResource,
                            Rc<IBackendImageView> view)
      : D3D11DeviceChild<ID3D11ShaderResourceView1>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_imageView(std::move(view)) {
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, m_imageView.get());
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      if (m_resource)
        m_resource->GetType(&dim);
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && static_cast<D3D11Texture2D*>(m_resource)->GetMsaaShadow())
        m_forcedMsaaTexture = static_cast<D3D11Texture2D*>(m_resource);
    }

    D3D11ShaderResourceView(D3D11Device* pDevice,
                            const D3D11_SHADER_RESOURCE_VIEW_DESC& desc,
                            ID3D11Resource* pResource,
                            Rc<IBackendBufferView> view)
      : D3D11DeviceChild<ID3D11ShaderResourceView1>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_bufferView(std::move(view)) {
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, nullptr);
      if (m_resource && m_bufferView)
        static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(m_resource))->RegisterBackendBufferView(m_bufferView.get());
    }

    ~D3D11ShaderResourceView() {
      if (m_resource && m_bufferView)
        static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(m_resource))->UnregisterBackendBufferView(m_bufferView.get());
      releaseResourcePrivateRef(m_resource);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11View)
       || riid == __uuidof(ID3D11ShaderResourceView)
       || riid == __uuidof(ID3D11ShaderResourceView1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10View)
       || riid == __uuidof(ID3D10ShaderResourceView)
       || riid == __uuidof(ID3D10ShaderResourceView1)) {
        *ppvObject = static_cast<ID3D10ShaderResourceView1*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    // The resource without a new reference; the view keeps it alive.
    ID3D11Resource* GetResourceRaw() const { return static_cast<ID3D11Resource*>(this->bindData(0)); }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) override {
      *ppResource = m_resource;
      if (m_resource) m_resource->AddRef();
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    // Planar formats are not supported, so the plane slice is always 0.
    void STDMETHODCALLTYPE GetDesc1(D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc) override {
      std::memset(pDesc, 0, sizeof(*pDesc));
      pDesc->Format        = m_desc.Format;
      pDesc->ViewDimension = m_desc.ViewDimension;
      switch (m_desc.ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
          pDesc->Texture2D.MostDetailedMip = m_desc.Texture2D.MostDetailedMip;
          pDesc->Texture2D.MipLevels       = m_desc.Texture2D.MipLevels;
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
          pDesc->Texture2DArray.MostDetailedMip = m_desc.Texture2DArray.MostDetailedMip;
          pDesc->Texture2DArray.MipLevels       = m_desc.Texture2DArray.MipLevels;
          pDesc->Texture2DArray.FirstArraySlice = m_desc.Texture2DArray.FirstArraySlice;
          pDesc->Texture2DArray.ArraySize       = m_desc.Texture2DArray.ArraySize;
          break;
        default:
          // The other members have the same layout in both structures.
          std::memcpy(&pDesc->Buffer, &m_desc.Buffer, sizeof(m_desc) - offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer));
          break;
      }
    }

    IBackendImageView* GetBackendView() const { return static_cast<IBackendImageView*>(this->bindData(1)); }
    IBackendBufferView* GetBackendBufferView() const { return m_bufferView.get(); }
    // The texture viewed when it has a forced multisampled copy.
    D3D11Texture2D* GetForcedMsaaTexture() const { return m_forcedMsaaTexture; }
    bool ReadsStencil() const {
      return m_desc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || m_desc.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    }

  private:
    D3D11_SHADER_RESOURCE_VIEW_DESC m_desc;
    ID3D11Resource*                 m_resource;
    Rc<IBackendImageView>           m_imageView;
    Rc<IBackendBufferView>          m_bufferView;
    D3D11Texture2D*                 m_forcedMsaaTexture = nullptr;

    D3D10ShaderResourceView m_d3d10{this};
  };

  // ============================================================
  // D3D11RenderTargetView
  // ============================================================

  class D3D11RenderTargetView : public D3D11DeviceChild<ID3D11RenderTargetView1> {
  public:
    D3D11RenderTargetView(D3D11Device* pDevice,
                          const D3D11_RENDER_TARGET_VIEW_DESC& desc,
                          ID3D11Resource* pResource,
                          Rc<IBackendImageView> view,
                          Rc<IBackendImageView> shadowView = nullptr)
      : D3D11DeviceChild<ID3D11RenderTargetView1>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_view(std::move(view))
      , m_shadowView(std::move(shadowView)) {
      if (IBackendImage* shadow = m_shadowView ? m_shadowView->getImage() : nullptr)
        m_shadowSampleCount = shadow->getDesc().sampleCount;
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, m_view.get());
      DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
      if (IBackendImage* image = m_view ? m_view->getImage() : nullptr) {
        resourceFormat = image->getDesc().format;
        m_sampleCount = image->getDesc().sampleCount ? image->getDesc().sampleCount : 1u;
      }
      m_pipelineFormat = validateImageViewFormat(m_desc.Format, resourceFormat);
    }

    ~D3D11RenderTargetView() {
      releaseResourcePrivateRef(m_resource);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11View)
       || riid == __uuidof(ID3D11RenderTargetView)
       || riid == __uuidof(ID3D11RenderTargetView1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10View)
       || riid == __uuidof(ID3D10RenderTargetView)) {
        *ppvObject = static_cast<ID3D10RenderTargetView*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    // The resource without a new reference; the view keeps it alive.
    ID3D11Resource* GetResourceRaw() const { return static_cast<ID3D11Resource*>(this->bindData(0)); }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) override {
      *ppResource = m_resource;
      if (m_resource) m_resource->AddRef();
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_RENDER_TARGET_VIEW_DESC1* pDesc) override {
      std::memset(pDesc, 0, sizeof(*pDesc));
      pDesc->Format        = m_desc.Format;
      pDesc->ViewDimension = m_desc.ViewDimension;
      switch (m_desc.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE2D:
          pDesc->Texture2D.MipSlice = m_desc.Texture2D.MipSlice;
          break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
          pDesc->Texture2DArray.MipSlice        = m_desc.Texture2DArray.MipSlice;
          pDesc->Texture2DArray.FirstArraySlice = m_desc.Texture2DArray.FirstArraySlice;
          pDesc->Texture2DArray.ArraySize       = m_desc.Texture2DArray.ArraySize;
          break;
        default:
          std::memcpy(&pDesc->Buffer, &m_desc.Buffer, sizeof(m_desc) - offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Buffer));
          break;
      }
    }

    IBackendImageView* GetBackendView() const { return static_cast<IBackendImageView*>(this->bindData(1)); }

    // The format a pipeline renders to this view with, and the sample count
    // of its image (0 without one).
    DXGI_FORMAT GetPipelineFormat() const { return m_pipelineFormat; }
    uint32_t GetSampleCount() const { return m_sampleCount; }

    // The view of the multisampled copy of forced multisampling, which the
    // immediate context draws into, and its sample count.
    IBackendImageView* GetShadowView() const { return m_shadowView.get(); }
    uint32_t GetShadowSampleCount() const { return m_shadowSampleCount; }

  private:
    D3D11_RENDER_TARGET_VIEW_DESC m_desc;
    ID3D11Resource*               m_resource;
    Rc<IBackendImageView>         m_view;
    Rc<IBackendImageView>         m_shadowView;
    uint32_t                      m_shadowSampleCount = 0;
    DXGI_FORMAT                   m_pipelineFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t                      m_sampleCount = 0;

    D3D10RenderTargetView m_d3d10{this};
  };

  // ============================================================
  // D3D11DepthStencilView
  // ============================================================

  class D3D11DepthStencilView : public D3D11DeviceChild<ID3D11DepthStencilView> {
  public:
    D3D11DepthStencilView(D3D11Device* pDevice,
                          const D3D11_DEPTH_STENCIL_VIEW_DESC& desc,
                          ID3D11Resource* pResource,
                          Rc<IBackendImageView> view,
                          Rc<IBackendImageView> shadowView = nullptr)
      : D3D11DeviceChild<ID3D11DepthStencilView>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_view(std::move(view))
      , m_shadowView(std::move(shadowView)) {
      if (IBackendImage* shadow = m_shadowView ? m_shadowView->getImage() : nullptr)
        m_shadowSampleCount = shadow->getDesc().sampleCount;
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, m_view.get());
      DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
      if (IBackendImage* image = m_view ? m_view->getImage() : nullptr) {
        resourceFormat = image->getDesc().format;
        m_sampleCount = image->getDesc().sampleCount ? image->getDesc().sampleCount : 1u;
      }
      m_pipelineFormat = resolveDepthStencilFormat(validateImageViewFormat(m_desc.Format, resourceFormat));
    }

    ~D3D11DepthStencilView() {
      releaseResourcePrivateRef(m_resource);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11View)
       || riid == __uuidof(ID3D11DepthStencilView)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10View)
       || riid == __uuidof(ID3D10DepthStencilView)) {
        *ppvObject = static_cast<ID3D10DepthStencilView*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    // The resource without a new reference; the view keeps it alive.
    ID3D11Resource* GetResourceRaw() const { return static_cast<ID3D11Resource*>(this->bindData(0)); }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) override {
      *ppResource = m_resource;
      if (m_resource) m_resource->AddRef();
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    IBackendImageView* GetBackendView() const { return static_cast<IBackendImageView*>(this->bindData(1)); }

    // The format a pipeline uses this view with, and the sample count of
    // its image (0 without one).
    DXGI_FORMAT GetPipelineFormat() const { return m_pipelineFormat; }
    uint32_t GetSampleCount() const { return m_sampleCount; }

    // The view of the multisampled copy of forced multisampling, and its
    // sample count.
    IBackendImageView* GetShadowView() const { return m_shadowView.get(); }
    uint32_t GetShadowSampleCount() const { return m_shadowSampleCount; }
    // Draws through a read-only view write nothing.
    bool IsReadOnly() const {
      const UINT readOnly = m_desc.Flags & (D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL);
      return (readOnly & D3D11_DSV_READ_ONLY_DEPTH) && (!HasStencil() || (readOnly & D3D11_DSV_READ_ONLY_STENCIL));
    }
    bool IsDepthReadOnly() const { return (m_desc.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0; }
    bool IsStencilReadOnly() const { return (m_desc.Flags & D3D11_DSV_READ_ONLY_STENCIL) != 0; }
    bool HasStencil() const {
      return m_pipelineFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || m_pipelineFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }

  private:
    D3D11_DEPTH_STENCIL_VIEW_DESC m_desc;
    ID3D11Resource*               m_resource;
    Rc<IBackendImageView>         m_view;
    Rc<IBackendImageView>         m_shadowView;
    uint32_t                      m_shadowSampleCount = 0;
    DXGI_FORMAT                   m_pipelineFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t                      m_sampleCount = 0;

    D3D10DepthStencilView m_d3d10{this};
  };

  // ============================================================
  // D3D11UnorderedAccessView
  // ============================================================

  class D3D11UnorderedAccessView : public D3D11DeviceChild<ID3D11UnorderedAccessView1> {
  public:
    D3D11UnorderedAccessView(D3D11Device* pDevice,
                             const D3D11_UNORDERED_ACCESS_VIEW_DESC& desc,
                             ID3D11Resource* pResource,
                             Rc<IBackendImageView> view)
      : D3D11DeviceChild<ID3D11UnorderedAccessView1>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_imageView(std::move(view)) {
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, m_imageView.get());
    }

    D3D11UnorderedAccessView(D3D11Device* pDevice,
                             const D3D11_UNORDERED_ACCESS_VIEW_DESC& desc,
                             ID3D11Resource* pResource,
                             Rc<IBackendBufferView> view)
      : D3D11DeviceChild<ID3D11UnorderedAccessView1>(pDevice)
      , m_desc(desc)
      , m_resource(pResource)
      , m_bufferView(std::move(view)) {
      addResourcePrivateRef(m_resource);
      this->setBindData(m_resource, nullptr);
      if (m_resource && m_bufferView)
        static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(m_resource))->RegisterBackendBufferView(m_bufferView.get());
    }

    ~D3D11UnorderedAccessView() {
      if (m_resource && m_bufferView)
        static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(m_resource))->UnregisterBackendBufferView(m_bufferView.get());
      releaseResourcePrivateRef(m_resource);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11View)
       || riid == __uuidof(ID3D11UnorderedAccessView)
       || riid == __uuidof(ID3D11UnorderedAccessView1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    // The resource without a new reference; the view keeps it alive.
    ID3D11Resource* GetResourceRaw() const { return static_cast<ID3D11Resource*>(this->bindData(0)); }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) override {
      *ppResource = m_resource;
      if (m_resource) m_resource->AddRef();
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_UNORDERED_ACCESS_VIEW_DESC1* pDesc) override {
      std::memset(pDesc, 0, sizeof(*pDesc));
      pDesc->Format        = m_desc.Format;
      pDesc->ViewDimension = m_desc.ViewDimension;
      switch (m_desc.ViewDimension) {
        case D3D11_UAV_DIMENSION_TEXTURE2D:
          pDesc->Texture2D.MipSlice = m_desc.Texture2D.MipSlice;
          break;
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          pDesc->Texture2DArray.MipSlice        = m_desc.Texture2DArray.MipSlice;
          pDesc->Texture2DArray.FirstArraySlice = m_desc.Texture2DArray.FirstArraySlice;
          pDesc->Texture2DArray.ArraySize       = m_desc.Texture2DArray.ArraySize;
          break;
        default:
          std::memcpy(&pDesc->Buffer, &m_desc.Buffer, sizeof(m_desc) - offsetof(D3D11_UNORDERED_ACCESS_VIEW_DESC, Buffer));
          break;
      }
    }

    IBackendImageView* GetBackendView() const { return static_cast<IBackendImageView*>(this->bindData(1)); }
    IBackendBufferView* GetBackendBufferView() const { return m_bufferView.get(); }

  private:
    D3D11_UNORDERED_ACCESS_VIEW_DESC m_desc;
    ID3D11Resource*                  m_resource;
    Rc<IBackendImageView>            m_imageView;
    Rc<IBackendBufferView>           m_bufferView;
  };

  // ============================================================
  // Shader wrapper classes
  // ============================================================

  // A shader's bytecode as graphics pipeline keys identify it.
  struct ShaderIdentity {
    uint64_t hash = 0;
    uint64_t size = 0;

    ShaderIdentity() = default;
    explicit ShaderIdentity(const IBackendShader* shader) {
      if (shader) {
        hash = shader->getBytecodeHash();
        size = shader->getBytecodeSize();
      }
    }
  };

  class D3D11VertexShader : public D3D11DeviceChild<ID3D11VertexShader> {
  public:
    D3D11VertexShader(
        D3D11Device*       pDevice,
        Rc<IBackendShader> shader,
        uint64_t           bytecodeHash,
        std::string        bindingSummary,
        std::string        inputSummary)
      : D3D11DeviceChild<ID3D11VertexShader>(pDevice)
      , m_shader(std::move(shader))
      , m_bytecodeHash(bytecodeHash)
      , m_bindingSummary(std::move(bindingSummary))
      , m_inputSummary(std::move(inputSummary)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11VertexShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10VertexShader)) {
        *ppvObject = static_cast<ID3D10VertexShader*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }
    const ShaderIdentity& GetIdentity() const { return m_identity; }
    uint64_t GetBytecodeHash() const { return m_bytecodeHash; }
    const std::string& GetBindingSummary() const { return m_bindingSummary; }
    const std::string& GetInputSummary() const { return m_inputSummary; }

  private:
    Rc<IBackendShader> m_shader;
    ShaderIdentity     m_identity{ m_shader.get() };
    uint64_t           m_bytecodeHash = 0ull;
    std::string        m_bindingSummary;
    std::string        m_inputSummary;

    D3D10VertexShader m_d3d10{this};
  };

  class D3D11PixelShader : public D3D11DeviceChild<ID3D11PixelShader> {
  public:
    D3D11PixelShader(
        D3D11Device*      pDevice,
        Rc<IBackendShader> shader,
        uint64_t          bytecodeHash,
        std::string       bindingSummary)
      : D3D11DeviceChild<ID3D11PixelShader>(pDevice)
      , m_shader(std::move(shader))
      , m_bytecodeHash(bytecodeHash)
      , m_bindingSummary(std::move(bindingSummary)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11PixelShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10PixelShader)) {
        *ppvObject = static_cast<ID3D10PixelShader*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }
    const ShaderIdentity& GetIdentity() const { return m_identity; }
    uint64_t GetBytecodeHash() const { return m_bytecodeHash; }
    const std::string& GetBindingSummary() const { return m_bindingSummary; }

  private:
    Rc<IBackendShader> m_shader;
    ShaderIdentity     m_identity{ m_shader.get() };
    uint64_t           m_bytecodeHash = 0ull;
    std::string        m_bindingSummary;

    D3D10PixelShader m_d3d10{this};
  };

  class D3D11GeometryShader : public D3D11DeviceChild<ID3D11GeometryShader> {
  public:
    D3D11GeometryShader(D3D11Device* pDevice, Rc<IBackendShader> shader)
      : D3D11DeviceChild<ID3D11GeometryShader>(pDevice)
      , m_shader(std::move(shader)) { }

    D3D11GeometryShader(D3D11Device* pDevice, Rc<IBackendShader> shader,
                         const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries,
                         const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream)
      : D3D11DeviceChild<ID3D11GeometryShader>(pDevice)
      , m_shader(std::move(shader))
      , m_rasterizedStream(RasterizedStream) {
      if (pSODeclaration && NumEntries > 0) {
        m_soEntries.resize(NumEntries);
        m_soSemanticNames.resize(NumEntries);
        for (UINT i = 0; i < NumEntries; i++) {
          m_soEntries[i] = pSODeclaration[i];
          m_soSemanticNames[i] = pSODeclaration[i].SemanticName ? pSODeclaration[i].SemanticName : "";
        }
        // Fix up pointers after all strings are stored (no more reallocations)
        for (UINT i = 0; i < NumEntries; i++) {
          m_soEntries[i].SemanticName = m_soSemanticNames[i].c_str();
        }
      }
      if (pBufferStrides && NumStrides > 0) {
        m_soStrides.assign(pBufferStrides, pBufferStrides + NumStrides);
      }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11GeometryShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10GeometryShader)) {
        *ppvObject = static_cast<ID3D10GeometryShader*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }
    const ShaderIdentity& GetIdentity() const { return m_identity; }
    bool HasStreamOutput() const { return !m_soEntries.empty(); }
    const std::vector<D3D11_SO_DECLARATION_ENTRY>& GetSOEntries() const { return m_soEntries; }
    const std::vector<UINT>& GetSOStrides() const { return m_soStrides; }
    UINT GetRasterizedStream() const { return m_rasterizedStream; }

  private:
    Rc<IBackendShader> m_shader;
    ShaderIdentity     m_identity{ m_shader.get() };
    std::vector<D3D11_SO_DECLARATION_ENTRY> m_soEntries;
    std::vector<std::string> m_soSemanticNames;
    std::vector<UINT> m_soStrides;
    UINT m_rasterizedStream = 0;

    D3D10GeometryShader m_d3d10{this};
  };

  class D3D11HullShader : public D3D11DeviceChild<ID3D11HullShader> {
  public:
    D3D11HullShader(D3D11Device* pDevice, Rc<IBackendShader> shader)
      : D3D11DeviceChild<ID3D11HullShader>(pDevice)
      , m_shader(std::move(shader)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11HullShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }
    const ShaderIdentity& GetIdentity() const { return m_identity; }

  private:
    Rc<IBackendShader> m_shader;
    ShaderIdentity     m_identity{ m_shader.get() };
  };

  class D3D11DomainShader : public D3D11DeviceChild<ID3D11DomainShader> {
  public:
    D3D11DomainShader(D3D11Device* pDevice, Rc<IBackendShader> shader)
      : D3D11DeviceChild<ID3D11DomainShader>(pDevice)
      , m_shader(std::move(shader)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11DomainShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }
    const ShaderIdentity& GetIdentity() const { return m_identity; }

  private:
    Rc<IBackendShader> m_shader;
    ShaderIdentity     m_identity{ m_shader.get() };
  };

  class D3D11ComputeShader : public D3D11DeviceChild<ID3D11ComputeShader> {
  public:
    D3D11ComputeShader(D3D11Device* pDevice, Rc<IBackendShader> shader,
                       Rc<IBackendPipeline> pipeline)
      : D3D11DeviceChild<ID3D11ComputeShader>(pDevice)
      , m_shader(std::move(shader))
      , m_pipeline(std::move(pipeline)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11ComputeShader)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    IBackendShader* GetBackendShader() const { return m_shader.get(); }

    // A compute PSO depends on nothing but the shader, so it is compiled at
    // creation like a native driver does. Null if that compile failed.
    const Rc<IBackendPipeline>& GetBackendPipeline() const { return m_pipeline; }

    // With asyncShaders the pipeline compiles in the background: the first
    // dispatch that got it keeps it here (the device keeps every pipeline).
    IBackendPipeline* GetCompiledPipeline() const { return m_compiled.load(std::memory_order_acquire); }
    void SetCompiledPipeline(IBackendPipeline* pipeline) { m_compiled.store(pipeline, std::memory_order_release); }

  private:
    Rc<IBackendShader> m_shader;
    Rc<IBackendPipeline> m_pipeline;
    std::atomic<IBackendPipeline*> m_compiled{nullptr};
  };

  // ============================================================
  // D3D11InputLayout
  // ============================================================

  class D3D11InputLayout : public D3D11DeviceChild<ID3D11InputLayout> {
  public:
    D3D11InputLayout(D3D11Device* pDevice,
                     std::vector<D3D11_INPUT_ELEMENT_DESC> elements,
                     std::vector<InputElementDesc> backendElements)
      : D3D11DeviceChild<ID3D11InputLayout>(pDevice)
      , m_elements(std::move(elements))
      , m_backendElements(std::move(backendElements)) {
      // D3D11 copies input-layout descriptions at creation time. Own the
      // semantic strings as well so both the PSO builder and its cache key stay
      // valid after the caller releases the original descriptors.
      m_semanticNames.resize(m_backendElements.size());
      for (size_t i = 0; i < m_backendElements.size(); i++)
        m_semanticNames[i] = m_backendElements[i].semanticName
          ? m_backendElements[i].semanticName
          : "";
      for (size_t i = 0; i < m_backendElements.size(); i++) {
        m_elements[i].SemanticName = m_semanticNames[i].c_str();
        m_backendElements[i].semanticName = m_semanticNames[i].c_str();
      }
      m_contentHash = hashInputLayout(m_backendElements);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11InputLayout)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10InputLayout)) {
        *ppvObject = static_cast<ID3D10InputLayout*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    const std::vector<D3D11_INPUT_ELEMENT_DESC>& GetElements() const { return m_elements; }

    const std::vector<InputElementDesc>& GetBackendElements() const { return m_backendElements; }
    uint64_t GetContentHash() const { return m_contentHash; }

  private:
    std::vector<D3D11_INPUT_ELEMENT_DESC> m_elements;
    std::vector<InputElementDesc>         m_backendElements;
    std::vector<std::string>              m_semanticNames;
    uint64_t                              m_contentHash = 0;

    D3D10InputLayout m_d3d10{this};
  };

  // ============================================================
  // State object wrappers
  // ============================================================

  class D3D11BlendState : public D3D11DeviceChild<ID3D11BlendState1> {
  public:
    D3D11BlendState(D3D11Device* pDevice,
                    const D3D11_BLEND_DESC& desc,
                    const BlendState& backendState)
      : D3D11DeviceChild<ID3D11BlendState1>(pDevice)
      , m_desc(desc)
      , m_desc1(upgradeBlendDesc1(&desc))
      , m_backendState(backendState)
      , m_backendHash(state_hash::blend(backendState)) { }

    D3D11BlendState(D3D11Device* pDevice,
                    const D3D11_BLEND_DESC1& desc,
                    const BlendState& backendState)
      : D3D11DeviceChild<ID3D11BlendState1>(pDevice)
      , m_desc(downgradeBlendDesc1(&desc))
      , m_desc1(desc)
      , m_backendState(backendState)
      , m_backendHash(state_hash::blend(backendState)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11BlendState)
       || riid == __uuidof(ID3D11BlendState1)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10BlendState)
       || riid == __uuidof(ID3D10BlendState1)) {
        *ppvObject = static_cast<ID3D10BlendState1*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_BLEND_DESC1* pDesc) override {
      *pDesc = m_desc1;
    }

    const BlendState& GetBackendState() const { return m_backendState; }
    uint64_t GetBackendHash() const { return m_backendHash; }

  private:
    D3D11_BLEND_DESC  m_desc;
    D3D11_BLEND_DESC1 m_desc1;
    BlendState        m_backendState;
    uint64_t m_backendHash;

    D3D10BlendState m_d3d10{this};
  };

  class D3D11DepthStencilState : public D3D11DeviceChild<ID3D11DepthStencilState> {
  public:
    D3D11DepthStencilState(D3D11Device* pDevice,
                           const D3D11_DEPTH_STENCIL_DESC& desc,
                           const DepthStencilState& backendState)
      : D3D11DeviceChild<ID3D11DepthStencilState>(pDevice)
      , m_desc(desc)
      , m_backendState(backendState)
      , m_backendHash(state_hash::depthStencil(backendState)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11DepthStencilState)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10DepthStencilState)) {
        *ppvObject = static_cast<ID3D10DepthStencilState*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    const DepthStencilState& GetBackendState() const { return m_backendState; }
    uint64_t GetBackendHash() const { return m_backendHash; }

    // Whether draws with this state can change the depth values of the
    // bound depth-stencil view.
    bool WritesDepth() const { return m_desc.DepthEnable && m_desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL; }
    bool WritesStencil() const {
      auto changes = [](const D3D11_DEPTH_STENCILOP_DESC& face) {
        return face.StencilFailOp != D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP
            || face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
      };
      return m_desc.StencilEnable && m_desc.StencilWriteMask && (changes(m_desc.FrontFace) || changes(m_desc.BackFace));
    }
    // Whether draws with this state access the depth-stencil view at all.
    bool UsesDepthStencil() const { return m_desc.DepthEnable || m_desc.StencilEnable; }
    bool UsesStencil() const { return m_desc.StencilEnable != FALSE; }

  private:
    D3D11_DEPTH_STENCIL_DESC m_desc;
    DepthStencilState        m_backendState;
    uint64_t m_backendHash;

    D3D10DepthStencilState m_d3d10{this};
  };

  class D3D11RasterizerState : public D3D11DeviceChild<ID3D11RasterizerState2> {
  public:
    D3D11RasterizerState(D3D11Device* pDevice,
                         const D3D11_RASTERIZER_DESC& desc,
                         const RasterizerState& backendState)
      : D3D11DeviceChild<ID3D11RasterizerState2>(pDevice)
      , m_desc(desc)
      , m_desc1(upgradeRasterizerDesc1(&desc))
      , m_backendState(backendState)
      , m_backendHash(state_hash::rasterizer(backendState)) { }

    D3D11RasterizerState(D3D11Device* pDevice,
                         const D3D11_RASTERIZER_DESC1& desc,
                         const RasterizerState& backendState)
      : D3D11DeviceChild<ID3D11RasterizerState2>(pDevice)
      , m_desc(downgradeRasterizerDesc1(&desc))
      , m_desc1(desc)
      , m_backendState(backendState)
      , m_backendHash(state_hash::rasterizer(backendState)) { }

    D3D11RasterizerState(D3D11Device* pDevice,
                         const D3D11_RASTERIZER_DESC2& desc,
                         const RasterizerState& backendState)
      : D3D11DeviceChild<ID3D11RasterizerState2>(pDevice)
      , m_backendState(backendState)
      , m_backendHash(state_hash::rasterizer(backendState)) {
      std::memcpy(&m_desc1, &desc, sizeof(m_desc1));
      m_desc = downgradeRasterizerDesc1(&m_desc1);
      m_conservativeRaster = desc.ConservativeRaster;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11RasterizerState)
       || riid == __uuidof(ID3D11RasterizerState1)
       || riid == __uuidof(ID3D11RasterizerState2)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10RasterizerState)) {
        *ppvObject = static_cast<ID3D10RasterizerState*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_RASTERIZER_DESC1* pDesc) override {
      *pDesc = m_desc1;
    }

    void STDMETHODCALLTYPE GetDesc2(D3D11_RASTERIZER_DESC2* pDesc) override {
      std::memcpy(pDesc, &m_desc1, sizeof(m_desc1));
      pDesc->ConservativeRaster = m_conservativeRaster;
    }

    const RasterizerState& GetBackendState() const { return m_backendState; }
    uint64_t GetBackendHash() const { return m_backendHash; }

  private:
    D3D11_RASTERIZER_DESC  m_desc;
    D3D11_RASTERIZER_DESC1 m_desc1;
    D3D11_CONSERVATIVE_RASTERIZATION_MODE m_conservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    RasterizerState        m_backendState;
    uint64_t m_backendHash;

    D3D10RasterizerState m_d3d10{this};
  };

  class D3D11SamplerState : public D3D11DeviceChild<ID3D11SamplerState> {
  public:
    D3D11SamplerState(D3D11Device* pDevice,
                      const D3D11_SAMPLER_DESC& desc,
                      Rc<IBackendSampler> sampler)
      : D3D11DeviceChild<ID3D11SamplerState>(pDevice)
      , m_desc(desc)
      , m_sampler(std::move(sampler)) {
      setBindData(m_sampler.get(), nullptr);
    }

    // Leaves the device's cache of sampler states.
    ~D3D11SamplerState();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11SamplerState)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10SamplerState)) {
        *ppvObject = static_cast<ID3D10SamplerState*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    IBackendSampler* GetBackendSampler() const { return static_cast<IBackendSampler*>(bindData(0)); }

  private:
    D3D11_SAMPLER_DESC  m_desc;
    Rc<IBackendSampler> m_sampler;

    D3D10SamplerState m_d3d10{this};
  };

  // ============================================================
  // D3D11Query
  // ============================================================

  // Shader class linkage. Dynamic shader linkage is not translated: like
  // DXVK, shaders created with a linkage object ignore it, and the object
  // only exists so applications that create one keep working.
  class D3D11ClassLinkage : public D3D11DeviceChild<ID3D11ClassLinkage> {
  public:
    explicit D3D11ClassLinkage(D3D11Device* pDevice)
    : D3D11DeviceChild<ID3D11ClassLinkage>(pDevice) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11ClassLinkage)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetClassInstance(
        LPCSTR               pClassInstanceName,
        UINT                 InstanceIndex,
        ID3D11ClassInstance** ppInstance) override {
      (void)pClassInstanceName;
      (void)InstanceIndex;
      if (ppInstance)
        *ppInstance = nullptr;
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE CreateClassInstance(
        LPCSTR               pClassTypeName,
        UINT                 ConstantBufferOffset,
        UINT                 ConstantVectorOffset,
        UINT                 TextureOffset,
        UINT                 SamplerOffset,
        ID3D11ClassInstance** ppInstance) override {
      (void)pClassTypeName;
      (void)ConstantBufferOffset;
      (void)ConstantVectorOffset;
      (void)TextureOffset;
      (void)SamplerOffset;
      if (ppInstance)
        *ppInstance = nullptr;
      return E_NOTIMPL;
    }
  };

  // ID3D11Fence over a D3D12 fence, which has the same semantics.
  class D3D11Fence : public D3D11DeviceChild<ID3D11Fence> {
  public:
    D3D11Fence(D3D11Device* pDevice, ComPtr<ID3D12Fence> fence)
    : D3D11DeviceChild<ID3D11Fence>(pDevice)
    , m_fence(std::move(fence)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Fence)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    // Defined after D3D11Device.
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        const SECURITY_ATTRIBUTES* pAttributes,
        DWORD                      dwAccess,
        LPCWSTR                    lpName,
        HANDLE*                    pHandle) override;

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
      return m_fence->GetCompletedValue();
    }

    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value, HANDLE hEvent) override {
      return m_fence->SetEventOnCompletion(Value, hEvent);
    }

    ID3D12Fence* GetNativeFence() const { return m_fence.Get(); }

  private:
    ComPtr<ID3D12Fence> m_fence;
  };

  class D3D11Query : public D3D11DeviceChild<ID3D11Query1> {
  public:
    D3D11Query(D3D11Device* pDevice,
               const D3D11_QUERY_DESC& desc,
               Rc<IBackendQuery> query)
      : D3D11DeviceChild<ID3D11Query1>(pDevice)
      , m_desc(desc)
      , m_query(std::move(query)) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11Asynchronous)
       || riid == __uuidof(ID3D11Query)
       || riid == __uuidof(ID3D11Query1)
       || (riid == __uuidof(ID3D11Predicate) && IsPredicate())) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      if (riid == __uuidof(ID3D10DeviceChild)
       || riid == __uuidof(ID3D10Asynchronous)
       || riid == __uuidof(ID3D10Query)
       || (riid == __uuidof(ID3D10Predicate) && IsPredicate())) {
        *ppvObject = static_cast<ID3D10Predicate*>(&m_d3d10);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    bool IsPredicate() const {
      return m_desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE
          || m_desc.Query == D3D11_QUERY_SO_OVERFLOW_PREDICATE
          || (m_desc.Query >= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0
           && m_desc.Query <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3
           && (m_desc.Query - D3D11_QUERY_SO_STATISTICS_STREAM0) % 2 == 1);
    }

    // GetData flushes the context once per End when results are not
    // submitted yet, not on every poll.
    bool RequestFlush() {
      if (m_flushRequested)
        return false;
      m_flushRequested = true;
      return true;
    }

    void ResetFlushRequest() { m_flushRequested = false; }

    UINT STDMETHODCALLTYPE GetDataSize() override {
      switch (m_desc.Query) {
        case D3D11_QUERY_EVENT:                         return sizeof(BOOL);
        case D3D11_QUERY_OCCLUSION:                     return sizeof(UINT64);
        case D3D11_QUERY_TIMESTAMP:                     return sizeof(UINT64);
        case D3D11_QUERY_TIMESTAMP_DISJOINT:            return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
        case D3D11_QUERY_PIPELINE_STATISTICS:           return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
        case D3D11_QUERY_OCCLUSION_PREDICATE:           return sizeof(BOOL);
        case D3D11_QUERY_SO_STATISTICS:                 return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE:         return sizeof(BOOL);
        case D3D11_QUERY_SO_STATISTICS_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3:         return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: return sizeof(BOOL);
        default:                                        return 0;
      }
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC* pDesc) override {
      *pDesc = m_desc;
    }

    void STDMETHODCALLTYPE GetDesc1(D3D11_QUERY_DESC1* pDesc) override {
      pDesc->Query       = m_desc.Query;
      pDesc->MiscFlags   = m_desc.MiscFlags;
      pDesc->ContextType = D3D11_CONTEXT_TYPE_ALL;
    }

    IBackendQuery* GetBackendQuery() const { return m_query.get(); }

  private:
    D3D11_QUERY_DESC   m_desc;
    Rc<IBackendQuery>  m_query;
    bool               m_flushRequested = false;

    D3D10Query m_d3d10{this};
  };

  // ============================================================
  // Interface to implementation mapping for context bindings
  // ============================================================

  template<typename T> struct D3D11ImplOf;
  template<> struct D3D11ImplOf<ID3D11Buffer>              { using Type = D3D11Buffer; };
  template<> struct D3D11ImplOf<ID3D11ShaderResourceView>  { using Type = D3D11ShaderResourceView; };
  template<> struct D3D11ImplOf<ID3D11RenderTargetView>    { using Type = D3D11RenderTargetView; };
  template<> struct D3D11ImplOf<ID3D11DepthStencilView>    { using Type = D3D11DepthStencilView; };
  template<> struct D3D11ImplOf<ID3D11UnorderedAccessView> { using Type = D3D11UnorderedAccessView; };
  template<> struct D3D11ImplOf<ID3D11InputLayout>         { using Type = D3D11InputLayout; };
  template<> struct D3D11ImplOf<ID3D11BlendState>          { using Type = D3D11BlendState; };
  template<> struct D3D11ImplOf<ID3D11DepthStencilState>   { using Type = D3D11DepthStencilState; };
  template<> struct D3D11ImplOf<ID3D11RasterizerState>     { using Type = D3D11RasterizerState; };
  template<> struct D3D11ImplOf<ID3D11SamplerState>        { using Type = D3D11SamplerState; };

  // ============================================================
  // D3D11CommandList - for deferred context support
  // ============================================================

  class D3D11CommandList : public ID3D11CommandList {
  public:
    // Like every device child, a command list keeps its device alive.
    D3D11CommandList(D3D11Device* pDevice, Rc<IBackendCommandList> commands,
                     std::vector<RecordedDiscard> discards = {},
                     std::vector<RecordedMsaaUse> msaaUses = {});
    ~D3D11CommandList();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      if (!ppvObject) return E_POINTER;
      *ppvObject = nullptr;
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(ID3D11CommandList)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting())
        delete this;
      return ref;
    }

    // ID3D11DeviceChild
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
      return m_privateData.getData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
      return m_privateData.setData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
      return m_privateData.setInterface(guid, pData);
    }

    // ID3D11CommandList
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }

    const Rc<IBackendCommandList>& GetBackendCommandList() const { return m_commands; }
    const std::vector<RecordedDiscard>& GetDiscards() const { return m_discards; }
    const std::vector<RecordedMsaaUse>& GetMsaaUses() const { return m_msaaUses; }

  private:
    std::atomic<ULONG> m_refCount;
    D3D11Device*       m_device;
    Rc<IBackendCommandList> m_commands;
    std::vector<RecordedDiscard> m_discards;
    std::vector<RecordedMsaaUse> m_msaaUses;
    ComPrivateData     m_privateData;
  };

  // ============================================================
  // D3D11Device - COM implementation of ID3D11Device
  // ============================================================

  class D3D11Device;

  // Private interface ID DXUP's D3D11Device answers with itself, so that
  // objects of other implementations (native D3D11 devices, D3D12 command
  // queues, wrappers of either) are never taken for it.
  static constexpr GUID kDxupD3D11DeviceIid =
    { 0x5c2e0c4b, 0x6f61, 0x4a57, { 0x9d, 0x8e, 0x2b, 0x5b, 0x3f, 0x1d, 0x7a, 0x10 } };

  // ID3D10Multithread / ID3D11Multithread (same method layout): with
  // protection on, every immediate context call holds the device lock, and
  // Enter/Leave hold it across several calls, as in native D3D11.
  class D3D11Multithread final : public ID3D11Multithread {
  public:
    explicit D3D11Multithread(IUnknown* owner) : m_owner(owner) { }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
      return m_owner->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

    void STDMETHODCALLTYPE Enter() override { m_mutex.lock(); }
    void STDMETHODCALLTYPE Leave() override { m_mutex.unlock(); }

    BOOL STDMETHODCALLTYPE SetMultithreadProtected(BOOL bMTProtect) override {
      return m_protected.exchange(bMTProtect != FALSE) ? TRUE : FALSE;
    }

    BOOL STDMETHODCALLTYPE GetMultithreadProtected() override {
      return m_protected.load() ? TRUE : FALSE;
    }

    bool isProtected() const { return m_protected.load(std::memory_order_relaxed); }
    std::recursive_mutex& mutex() { return m_mutex; }

  private:
    IUnknown*               m_owner;
    std::recursive_mutex    m_mutex;
    std::atomic<bool>       m_protected{false};
  };

  class D3D11Device : public ID3D11Device5, public ID3D11On12Device {
  public:
    D3D11Device(BackendType backendType, D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0,
                const LUID* adapterLuid = nullptr)
      : m_refCount(1)
      , m_backendDevice(backendType, adapterLuid)
      , m_featureLevel(featureLevel) { }

    // Defined in d3d11_main.cpp after D3D11ImmediateContext is complete
    ~D3D11Device();

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      ULONG ref = --m_refCount;
      if (ref == 0 && !processExiting())
        delete this;
      return ref;
    }

    // ---- ID3D11Device: Resource Creation ----

    // The checks D3D11 applies to buffer descriptions, as the native
    // runtime answers them.
    static HRESULT validateBufferDesc(const D3D11_BUFFER_DESC& desc, bool initialData) {
      const UINT bind = desc.BindFlags;
      const UINT misc = desc.MiscFlags;
      const UINT textureMisc = D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE
        | D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
      if (!desc.ByteWidth || (bind & D3D11_BIND_DEPTH_STENCIL) || (misc & textureMisc))
        return E_INVALIDARG;
      // Constant buffers are whole constants and bound as nothing else.
      if ((bind & D3D11_BIND_CONSTANT_BUFFER)
       && ((bind & ~UINT(D3D11_BIND_CONSTANT_BUFFER)) || (desc.ByteWidth & 15)))
        return E_INVALIDARG;
      if (misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
        const UINT stride = desc.StructureByteStride;
        const UINT fixedFunctionBinds = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER
          | D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_STREAM_OUTPUT;
        if (!stride || (stride & 3) || stride > 2048 || desc.ByteWidth % stride
         || (misc & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || (bind & fixedFunctionBinds))
          return E_INVALIDARG;
      }
      if ((misc & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)
       && !(bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)))
        return E_INVALIDARG;
      // Buffers are shared through legacy handles only, with one kind of
      // synchronization, and not dynamic ones.
      const UINT sharedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      if ((misc & sharedMisc) == sharedMisc || (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
       || ((misc & sharedMisc) && desc.Usage == D3D11_USAGE_DYNAMIC))
        return E_INVALIDARG;

      const UINT gpuWrites = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET;
      switch (desc.Usage) {
        case D3D11_USAGE_DEFAULT:
          return desc.CPUAccessFlags ? E_INVALIDARG : S_OK;
        case D3D11_USAGE_IMMUTABLE:
          return initialData && !desc.CPUAccessFlags && !(bind & gpuWrites) ? S_OK : E_INVALIDARG;
        case D3D11_USAGE_DYNAMIC:
          return desc.CPUAccessFlags == D3D11_CPU_ACCESS_WRITE && bind && !(bind & gpuWrites) ? S_OK : E_INVALIDARG;
        case D3D11_USAGE_STAGING:
          return !bind && desc.CPUAccessFlags ? S_OK : E_INVALIDARG;
        default:
          return E_INVALIDARG;
      }
    }

    HRESULT STDMETHODCALLTYPE CreateBuffer(
        const D3D11_BUFFER_DESC*      pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Buffer**                ppBuffer) override {
      if (!pDesc) return E_INVALIDARG;
      if (HRESULT hr = validateBufferDesc(*pDesc, pInitialData && pInitialData->pSysMem); FAILED(hr))
        return rejectInvalidCall("validateBufferDesc", hr);
      if (!ppBuffer) return S_FALSE;

      logMessage(LogLevel::Trace, "CreateBuffer(Size=%u, BindFlags=0x%x, Usage=%d, CPUAccess=0x%x)",
        pDesc->ByteWidth, pDesc->BindFlags, pDesc->Usage, pDesc->CPUAccessFlags);

      // A staging buffer is never bound to the pipeline and is only touched
      // by Map and copies, so its D3D12 resource is created on first use.
      // Applications often create staging buffers they never use, and NvAPI
      // probes the driver device behind an ID3D11Device by watching which
      // driver device creates the next resource during CreateBuffer; that
      // must not find the D3D12 device, whose D3D11 entry points are null.
      const bool deferAllocation = pDesc->Usage == D3D11_USAGE_STAGING
        && !(pInitialData && pInitialData->pSysMem);

      Rc<IBackendBuffer> backendBuffer;
      if (!deferAllocation) {
        backendBuffer = m_backendDevice.createBuffer(pDesc, pInitialData);
        if (!backendBuffer) {
          logMessage(LogLevel::Error, "CreateBuffer failed: backend returned null");
          return E_FAIL;
        }
      }

      auto* wrappedBuffer = new D3D11Buffer(
        this,
        *pDesc,
        std::move(backendBuffer),
        pInitialData);
      *ppBuffer = wrappedBuffer;
      if (pDesc->MiscFlags & kResourceMiscShared) {
        auto* backend = static_cast<d3d12::D3D12BackendBuffer*>(wrappedBuffer->GetBackendBuffer());
        makeShared(wrappedBuffer->dxgiResource(), D3D11_RESOURCE_DIMENSION_BUFFER, pDesc, sizeof(*pDesc),
          backend ? backend->sharedSync() : nullptr);
      }

      const UINT iaFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
      if ((pDesc->BindFlags & iaFlags) && (pDesc->ByteWidth >= (1u << 20) || (pDesc->BindFlags & iaFlags) == iaFlags)) {
        logMessage(
          LogLevel::Info,
          "CreateBuffer-ia: buf=%p size=%u bind=0x%x usage=%u cpu=0x%x misc=0x%x stride=%u trackBytes=%llu initial=%u",
          wrappedBuffer,
          pDesc->ByteWidth,
          pDesc->BindFlags,
          static_cast<uint32_t>(pDesc->Usage),
          pDesc->CPUAccessFlags,
          pDesc->MiscFlags,
          pDesc->StructureByteStride,
          static_cast<unsigned long long>(wrappedBuffer->GetShadowSize()),
          pInitialData && pInitialData->pSysMem ? 1u : 0u);
      }

      return S_OK;
    }

    // Refused views with what they asked for: a check stricter than the
    // native runtime shows which description it refused.
    template<typename Desc>
    static HRESULT rejectInvalidView(const char* check, HRESULT hr, ID3D11Resource* resource, const Desc* desc) {
      ViewResourceInfo info;
      getViewResourceInfo(resource, info);
      // The first six words of the description; shorter ones end in zeros.
      UINT words[6] = {};
      if (desc)
        std::memcpy(words, desc, (std::min)(sizeof(words), sizeof(Desc)));
      static std::atomic<uint32_t> s_count = { 0 };
      if (s_count.fetch_add(1, std::memory_order_relaxed) < 32) {
        logMessage(LogLevel::Warn,
          "%s refused: resource dim=%u fmt=%u %ux%ux%u mips=%u array=%u samples=%u bind=0x%x misc=0x%x bytes=%u; "
          "view %s fmt=%u dim=%u args=%u,%u,%u,%u",
          check, unsigned(info.dimension), unsigned(info.format), info.width, info.height, info.depth, info.mipLevels,
          info.arraySize, info.samples, info.bindFlags, info.miscFlags, info.byteWidth,
          desc ? "desc" : "default", desc ? unsigned(desc->Format) : 0u, desc ? unsigned(desc->ViewDimension) : 0u,
          words[2], words[3], words[4], words[5]);
      }
      return rejectInvalidCall(check, hr);
    }

    // Calls refused like D3D11 refuses them, noted so that a check stricter
    // than the native runtime shows in the log of a game.
    static HRESULT rejectInvalidCall(const char* check, HRESULT hr) {
      static std::atomic<uint32_t> s_count = { 0 };
      if (s_count.fetch_add(1, std::memory_order_relaxed) < 64)
        logMessage(LogLevel::Warn, "Invalid call refused by %s (hr=0x%08x)", check, static_cast<uint32_t>(hr));
      return hr;
    }

    // The checks D3D11 applies to texture descriptions, as the native
    // runtime answers them: E_INVALIDARG for descriptions it rejects.
    HRESULT validateTextureDesc(
        D3D11_RESOURCE_DIMENSION dimension,
        UINT                     width,
        UINT                     height,
        UINT                     depth,
        UINT                     mipLevels,
        UINT                     arraySize,
        DXGI_FORMAT              format,
        DXGI_SAMPLE_DESC         sampleDesc,
        D3D11_USAGE              usage,
        UINT                     bindFlags,
        UINT                     cpuAccessFlags,
        UINT                     miscFlags,
        bool                     initialData) {
      const bool is3D = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
      const UINT maxExtent = is3D ? D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
        : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? D3D11_REQ_TEXTURE1D_U_DIMENSION
        : D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
      if (format == DXGI_FORMAT_UNKNOWN
       || !width || !height || !depth || !arraySize
       || width > maxExtent || height > maxExtent || depth > maxExtent
       || arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION
       || mipLevels > computeFullMipLevelCount(width, height, depth))
        return E_INVALIDARG;

      // Buffer bindings, and depth together with color.
      const UINT bufferBinds = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER
        | D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_STREAM_OUTPUT;
      if ((bindFlags & bufferBinds)
       || ((bindFlags & D3D11_BIND_RENDER_TARGET) && (bindFlags & D3D11_BIND_DEPTH_STENCIL)))
        return E_INVALIDARG;

      // What the format supports; typeless formats take the bindings of
      // the formats they are viewed as.
      D3D11_FEATURE_DATA_FORMAT_SUPPORT support = { format };
      if (FAILED(CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
       || !support.OutFormatSupport)
        return E_INVALIDARG;
      const UINT formatSupport = support.OutFormatSupport;
      const UINT dimensionSupport = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? D3D11_FORMAT_SUPPORT_TEXTURE1D
        : is3D ? D3D11_FORMAT_SUPPORT_TEXTURE3D : D3D11_FORMAT_SUPPORT_TEXTURE2D;
      if (!(formatSupport & dimensionSupport))
        return E_INVALIDARG;
      if (!isTypelessImageFormat(format)) {
        if (((bindFlags & D3D11_BIND_RENDER_TARGET) && !(formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
         || ((bindFlags & D3D11_BIND_DEPTH_STENCIL) && !(formatSupport & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
         || ((bindFlags & D3D11_BIND_SHADER_RESOURCE) && !(formatSupport & D3D11_FORMAT_SUPPORT_SHADER_LOAD)))
          return E_INVALIDARG;
      }
      // Block-compressed images consist of whole blocks.
      if (isBlockCompressedFormat(format) && ((width & 3) || (height & 3)))
        return E_INVALIDARG;

      if ((miscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS)
       && (bindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
          != (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        return E_INVALIDARG;
      if ((miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
       && (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || width != height || arraySize < 6))
        return E_INVALIDARG;

      if (!sampleDesc.Count)
        return E_INVALIDARG;
      // Sharing: one kind of synchronization; NT handles for single-sample
      // 2D textures that are no depth buffers; no dynamic shared textures.
      const UINT sharedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      if ((miscFlags & sharedMisc) == sharedMisc
       || ((miscFlags & sharedMisc) && usage == D3D11_USAGE_DYNAMIC))
        return E_INVALIDARG;
      if ((miscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
       && (!(miscFlags & sharedMisc) || dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || sampleDesc.Count > 1
        || (bindFlags & D3D11_BIND_DEPTH_STENCIL) || usage == D3D11_USAGE_STAGING
        || (miscFlags & D3D11_RESOURCE_MISC_TILED)))
        return E_INVALIDARG;
      if (sampleDesc.Count > 1) {
        UINT qualityLevels = 0;
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || mipLevels != 1
         || (bindFlags & D3D11_BIND_UNORDERED_ACCESS)
         || FAILED(CheckMultisampleQualityLevels(format, sampleDesc.Count, &qualityLevels))
         || sampleDesc.Quality >= qualityLevels)
          return E_INVALIDARG;
      }

      const UINT gpuWrites = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
      switch (usage) {
        case D3D11_USAGE_DEFAULT:
          // CPU access to default textures needs MapOnDefaultTextures.
          return cpuAccessFlags ? E_INVALIDARG : S_OK;
        case D3D11_USAGE_IMMUTABLE:
          return initialData && !cpuAccessFlags && !(bindFlags & gpuWrites) ? S_OK : E_INVALIDARG;
        case D3D11_USAGE_DYNAMIC:
          return cpuAccessFlags == D3D11_CPU_ACCESS_WRITE && mipLevels == 1 && arraySize == 1
              && bindFlags && !(bindFlags & gpuWrites) && !(miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
            ? S_OK : E_INVALIDARG;
        case D3D11_USAGE_STAGING:
          return !bindFlags && cpuAccessFlags && sampleDesc.Count == 1 ? S_OK : E_INVALIDARG;
        default:
          return E_INVALIDARG;
      }
    }

    HRESULT STDMETHODCALLTYPE CreateTexture1D(
        const D3D11_TEXTURE1D_DESC*   pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture1D**             ppTexture1D) override {
      if (!pDesc) return E_INVALIDARG;
      if (HRESULT hr = validateTextureDesc(D3D11_RESOURCE_DIMENSION_TEXTURE1D, pDesc->Width, 1, 1,
            pDesc->MipLevels, pDesc->ArraySize, pDesc->Format, DXGI_SAMPLE_DESC{ 1, 0 }, pDesc->Usage,
            pDesc->BindFlags, pDesc->CPUAccessFlags, pDesc->MiscFlags, pInitialData != nullptr); FAILED(hr))
        return rejectInvalidCall("validateTextureDesc", hr);
      if (!ppTexture1D) return S_FALSE;

      D3D11_TEXTURE1D_DESC effectiveDesc = *pDesc;
      if (!effectiveDesc.MipLevels)
        effectiveDesc.MipLevels = computeFullMipLevelCount(effectiveDesc.Width, 1u, 1u);

      auto backendImage = m_backendDevice.createTexture1D(pDesc, pInitialData);
      if (!backendImage) return E_FAIL;

      auto* texture = new D3D11Texture1D(this, effectiveDesc, std::move(backendImage));
      if (effectiveDesc.MiscFlags & kResourceMiscShared)
        makeShared(texture->dxgiResource(), D3D11_RESOURCE_DIMENSION_TEXTURE1D, &effectiveDesc, sizeof(effectiveDesc),
          static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage())->sharedSync());
      *ppTexture1D = texture;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateTexture2D(
        const D3D11_TEXTURE2D_DESC*   pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture2D**             ppTexture2D) override {
      if (!pDesc) return E_INVALIDARG;
      if (HRESULT hr = validateTextureDesc(D3D11_RESOURCE_DIMENSION_TEXTURE2D, pDesc->Width, pDesc->Height, 1,
            pDesc->MipLevels, pDesc->ArraySize, pDesc->Format, pDesc->SampleDesc, pDesc->Usage,
            pDesc->BindFlags, pDesc->CPUAccessFlags, pDesc->MiscFlags, pInitialData != nullptr); FAILED(hr))
        return rejectInvalidCall("validateTextureDesc", hr);
      if (!ppTexture2D) return S_FALSE;

      auto isTrackedCreateTexture = [&](const D3D11_TEXTURE2D_DESC* desc) {
        if (!desc)
          return false;

        UINT mipLevels = desc->MipLevels ? desc->MipLevels : 1u;
        UINT arraySize = desc->ArraySize ? desc->ArraySize : 1u;
        if (mipLevels != 1u || arraySize != 1u)
          return false;

        switch (desc->Format) {
          case DXGI_FORMAT_BC1_TYPELESS:
          case DXGI_FORMAT_BC1_UNORM:
          case DXGI_FORMAT_BC1_UNORM_SRGB:
            return desc->Width == 4u && desc->Height == 4u;

          case DXGI_FORMAT_BC3_TYPELESS:
          case DXGI_FORMAT_BC3_UNORM:
          case DXGI_FORMAT_BC3_UNORM_SRGB:
            return desc->Width == 1560u && desc->Height == 908u;

          default:
            return false;
        }
      };

      auto getTrackedCreateLabel = [&](const D3D11_TEXTURE2D_DESC* desc) {
        switch (desc->Format) {
          case DXGI_FORMAT_BC1_TYPELESS:
          case DXGI_FORMAT_BC1_UNORM:
          case DXGI_FORMAT_BC1_UNORM_SRGB:
            return "final-bc1-4x4";

          case DXGI_FORMAT_BC3_TYPELESS:
          case DXGI_FORMAT_BC3_UNORM:
          case DXGI_FORMAT_BC3_UNORM_SRGB:
            return "final-bc3-1560x908";

          default:
            return "untracked";
        }
      };

      auto logTrackedCreateCallsite = [&](const char* tag) {
        void* frames[4] = {};
        USHORT frameCount = CaptureStackBackTrace(1, 4, frames, nullptr);
        logMessage(LogLevel::Info,
          "%s: kind=%s fmt=%u %ux%u mip=%u array=%u bind=0x%x usage=%u init=%u frames=%p %p %p %p",
          tag,
          getTrackedCreateLabel(pDesc),
          static_cast<uint32_t>(pDesc->Format),
          pDesc->Width,
          pDesc->Height,
          pDesc->MipLevels,
          pDesc->ArraySize,
          pDesc->BindFlags,
          pDesc->Usage,
          pInitialData ? 1u : 0u,
          frameCount > 0 ? frames[0] : nullptr,
          frameCount > 1 ? frames[1] : nullptr,
          frameCount > 2 ? frames[2] : nullptr,
          frameCount > 3 ? frames[3] : nullptr);
      };

      const bool trackedCreateTexture = d3d11InfoLoggingEnabled()
        && isTrackedCreateTexture(pDesc);

      if (trackedCreateTexture)
        logTrackedCreateCallsite("CreateTexture2D-callsite");

      logCreateTexture2DInitialFingerprint(pDesc, pInitialData);

      D3D11_TEXTURE2D_DESC effectiveDesc = *pDesc;
      if (!effectiveDesc.MipLevels)
        effectiveDesc.MipLevels = computeFullMipLevelCount(effectiveDesc.Width, effectiveDesc.Height, 1u);

      // Multisampled textures may get more samples than asked for; shared
      // ones keep what other devices expect. Forced multisampling refills
      // the multisampled copy from the texture with a shader that reads it.
      D3D11_TEXTURE2D_DESC backendDesc = *pDesc;
      ForcedMsaaFormats forcedMsaa;
      const bool forceMsaa = ShouldForceMsaa(effectiveDesc, forcedMsaa);
      if (forceMsaa)
        backendDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
      if (!(pDesc->MiscFlags & (kResourceMiscShared | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
        const UINT samples = EnhancedSampleCount(pDesc->Format, pDesc->SampleDesc.Count);
        if (samples != pDesc->SampleDesc.Count) {
          backendDesc.SampleDesc.Count = samples;
          backendDesc.SampleDesc.Quality = 0;
        }
      }

      auto backendImage = m_backendDevice.createTexture2D(&backendDesc, pInitialData);
      if (!backendImage) {
        logMessage(LogLevel::Error, "CreateTexture2D failed: backend returned null");
        return E_FAIL;
      }

      auto* texture = new D3D11Texture2D(this, effectiveDesc, std::move(backendImage));
      if (forceMsaa) {
        AttachMsaaShadow(texture, forcedMsaa);
        // Initial data is in the texture, not yet in the copy.
        if (pInitialData)
          texture->ResetMsaaState(D3D11Texture2D::MsaaState::ImageNewer);
      }
      if (effectiveDesc.MiscFlags & kResourceMiscShared)
        makeShared(texture->dxgiResource(), D3D11_RESOURCE_DIMENSION_TEXTURE2D, &effectiveDesc, sizeof(effectiveDesc),
          static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage())->sharedSync());
      *ppTexture2D = texture;

      if (trackedCreateTexture) {
        logMessage(LogLevel::Info,
          "CreateTexture2D-tracked: kind=%s tex=%p backendImg=%p fmt=%u %ux%u mip=%u array=%u bind=0x%x usage=%u init=%u pitch=%u slicePitch=%u",
          getTrackedCreateLabel(pDesc),
          texture,
          texture->GetBackendImage(),
          static_cast<uint32_t>(pDesc->Format),
          pDesc->Width,
          pDesc->Height,
          pDesc->MipLevels,
          pDesc->ArraySize,
          pDesc->BindFlags,
          pDesc->Usage,
          pInitialData ? 1u : 0u,
          pInitialData ? pInitialData[0].SysMemPitch : 0u,
          pInitialData ? pInitialData[0].SysMemSlicePitch : 0u);
      }

      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateTexture3D(
        const D3D11_TEXTURE3D_DESC*   pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture3D**             ppTexture3D) override {
      if (!pDesc) return E_INVALIDARG;
      if (HRESULT hr = validateTextureDesc(D3D11_RESOURCE_DIMENSION_TEXTURE3D, pDesc->Width, pDesc->Height,
            pDesc->Depth, pDesc->MipLevels, 1, pDesc->Format, DXGI_SAMPLE_DESC{ 1, 0 }, pDesc->Usage,
            pDesc->BindFlags, pDesc->CPUAccessFlags, pDesc->MiscFlags, pInitialData != nullptr); FAILED(hr))
        return rejectInvalidCall("validateTextureDesc", hr);
      if (!ppTexture3D) return S_FALSE;

      D3D11_TEXTURE3D_DESC effectiveDesc = *pDesc;
      if (!effectiveDesc.MipLevels)
        effectiveDesc.MipLevels = computeFullMipLevelCount(effectiveDesc.Width, effectiveDesc.Height, effectiveDesc.Depth);

      auto backendImage = m_backendDevice.createTexture3D(pDesc, pInitialData);
      if (!backendImage) return E_FAIL;

      auto* texture = new D3D11Texture3D(this, effectiveDesc, std::move(backendImage));
      if (effectiveDesc.MiscFlags & kResourceMiscShared)
        makeShared(texture->dxgiResource(), D3D11_RESOURCE_DIMENSION_TEXTURE3D, &effectiveDesc, sizeof(effectiveDesc),
          static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage())->sharedSync());
      *ppTexture3D = texture;
      return S_OK;
    }

    // ---- ID3D11Device: View Creation ----

    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D11Resource*                        pResource,
        const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
        ID3D11ShaderResourceView**             ppSRView) override {
      if (!pResource) return E_INVALIDARG;
      D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      if (HRESULT hr = resolveSrvDesc(pResource, pDesc, formatSupportQuery(), srvDesc); FAILED(hr))
        return rejectInvalidView("resolveSrvDesc", hr, pResource, pDesc);
      if (!ppSRView)  return S_FALSE;
      const D3D11_SHADER_RESOURCE_VIEW_DESC backendSrvDesc = getBackendSrvDesc(pResource, srvDesc);
      pDesc = &backendSrvDesc;

      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* bufferResource = static_cast<D3D11Buffer*>(pResource);
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferResource->GetDesc(&bufferDesc);

        BufferViewDesc viewDesc = {};
        if (pDesc) {
          if (!initBufferViewDescFromSrv(*pDesc, bufferDesc, viewDesc))
            return E_INVALIDARG;
        } else if (!initDefaultBufferViewDesc(bufferDesc, viewDesc)) {
          return E_INVALIDARG;
        }

        auto backendView = m_backendDevice.getBackend()->createBufferView(bufferResource->GetBackendBuffer(), viewDesc);
        if (!backendView) return E_FAIL;

        *ppSRView = new D3D11ShaderResourceView(this, srvDesc, pResource, std::move(backendView));
        return S_OK;
      }

      IBackendImage* backendImage = nullptr;

      switch (dim) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          backendImage = static_cast<D3D11Texture1D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          backendImage = static_cast<D3D11Texture2D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
          backendImage = static_cast<D3D11Texture3D*>(pResource)->GetBackendImage();
          break;
        default:
          return E_INVALIDARG;
      }

      const auto& imgDesc = backendImage->getDesc();
      ImageViewDesc viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::ShaderResource);
      if (pDesc && !initImageViewDescFromSrv(*pDesc, imgDesc, viewDesc))
        return E_INVALIDARG;

      auto backendView = m_backendDevice.getBackend()->createImageView(backendImage, viewDesc);
      if (!backendView) return E_FAIL;

      *ppSRView = new D3D11ShaderResourceView(this, srvDesc, pResource, std::move(backendView));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D11Resource*                         pResource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
        ID3D11UnorderedAccessView**             ppUAView) override {
      if (!pResource) return E_INVALIDARG;
      D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      if (HRESULT hr = resolveUavDesc(pResource, pDesc, formatSupportQuery(), uavDesc); FAILED(hr))
        return rejectInvalidView("resolveUavDesc", hr, pResource, pDesc);
      if (!ppUAView)  return S_FALSE;
      pDesc = &uavDesc;

      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* bufferResource = static_cast<D3D11Buffer*>(pResource);
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferResource->GetDesc(&bufferDesc);

        BufferViewDesc viewDesc = {};
        if (pDesc) {
          if (!initBufferViewDescFromUav(*pDesc, bufferDesc, viewDesc))
            return E_INVALIDARG;
        } else if (!initDefaultBufferViewDesc(bufferDesc, viewDesc)) {
          return E_INVALIDARG;
        }

        auto backendView = m_backendDevice.getBackend()->createBufferView(bufferResource->GetBackendBuffer(), viewDesc);
        if (!backendView) return E_FAIL;

        *ppUAView = new D3D11UnorderedAccessView(this, uavDesc, pResource, std::move(backendView));
        return S_OK;
      }

      IBackendImage* backendImage = nullptr;

      switch (dim) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          backendImage = static_cast<D3D11Texture1D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          backendImage = static_cast<D3D11Texture2D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
          backendImage = static_cast<D3D11Texture3D*>(pResource)->GetBackendImage();
          break;
        default:
          return E_INVALIDARG;
      }

      const auto& imgDesc = backendImage->getDesc();
      ImageViewDesc viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::UnorderedAccess);
      if (pDesc && !initImageViewDescFromUav(*pDesc, imgDesc, viewDesc))
        return E_INVALIDARG;

      auto backendView = m_backendDevice.getBackend()->createImageView(backendImage, viewDesc);
      if (!backendView) return E_FAIL;

      *ppUAView = new D3D11UnorderedAccessView(this, uavDesc, pResource, std::move(backendView));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D11Resource*                       pResource,
        const D3D11_RENDER_TARGET_VIEW_DESC*  pDesc,
        ID3D11RenderTargetView**              ppRTView) override {
      logMessage(LogLevel::Trace, "CreateRenderTargetView(Format=%d)",
        pDesc ? pDesc->Format : -1);
      if (!pResource) return E_INVALIDARG;
      D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
      if (HRESULT hr = resolveRtvDesc(pResource, pDesc, formatSupportQuery(), rtvDesc); FAILED(hr))
        return rejectInvalidView("resolveRtvDesc", hr, pResource, pDesc);
      if (!ppRTView)  return S_FALSE;
      pDesc = &rtvDesc;

      IBackendImage* backendImage = nullptr;
      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      switch (dim) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
          backendImage = static_cast<D3D11Texture1D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
          backendImage = static_cast<D3D11Texture2D*>(pResource)->GetBackendImage();
          break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
          backendImage = static_cast<D3D11Texture3D*>(pResource)->GetBackendImage();
          break;
        default:
          // Render target views of buffers are not translated.
          return E_INVALIDARG;
      }

      const auto& imgDesc = backendImage->getDesc();
      ImageViewDesc viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::RenderTarget);
      if (pDesc && !initImageViewDescFromRtv(*pDesc, imgDesc, viewDesc))
        return E_INVALIDARG;

      auto backendView = m_backendDevice.getBackend()->createImageView(backendImage, viewDesc);
      if (!backendView) return E_FAIL;

      // Forced multisampling: the immediate context draws into the
      // multisampled copy in the format of this view. Integer formats cannot
      // be resolved and draw into the texture itself.
      Rc<IBackendImageView> shadowView;
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        auto* texture = static_cast<D3D11Texture2D*>(pResource);
        const DXGI_FORMAT viewFormat = viewDesc.format != DXGI_FORMAT_UNKNOWN ? viewDesc.format : imgDesc.format;
        if (IBackendImage* shadow = texture->GetMsaaShadow(); shadow && isResolvableColorViewFormat(viewFormat)) {
          ImageViewDesc shadowDesc = viewDesc;
          shadowDesc.type = ImageViewType::View2DMS;
          shadowDesc.format = viewFormat;
          shadowView = m_backendDevice.getBackend()->createImageView(shadow, shadowDesc);
          if (shadowView)
            texture->SetMsaaColorFormat(viewFormat);
        }
      }

      *ppRTView = new D3D11RenderTargetView(this, rtvDesc, pResource, std::move(backendView), std::move(shadowView));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D11Resource*                       pResource,
        const D3D11_DEPTH_STENCIL_VIEW_DESC*  pDesc,
        ID3D11DepthStencilView**              ppDSView) override {
      if (!pResource) return E_INVALIDARG;
      D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
      if (HRESULT hr = resolveDsvDesc(pResource, pDesc, formatSupportQuery(), dsvDesc); FAILED(hr))
        return rejectInvalidView("resolveDsvDesc", hr, pResource, pDesc);
      if (!ppDSView)  return S_FALSE;
      pDesc = &dsvDesc;

      IBackendImage* backendImage = nullptr;
      D3D11_RESOURCE_DIMENSION dim;
      pResource->GetType(&dim);

      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        backendImage = static_cast<D3D11Texture2D*>(pResource)->GetBackendImage();
      else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        backendImage = static_cast<D3D11Texture1D*>(pResource)->GetBackendImage();
      else
        return E_INVALIDARG;

      const auto& imgDesc = backendImage->getDesc();
      ImageViewDesc viewDesc = getDefaultImageViewDesc(imgDesc, ImageViewUsage::DepthStencil);
      if (pDesc && !initImageViewDescFromDsv(*pDesc, imgDesc, viewDesc))
        return E_INVALIDARG;

      auto backendView = m_backendDevice.getBackend()->createImageView(backendImage, viewDesc);
      if (!backendView) return E_FAIL;

      // Forced multisampling: the immediate context draws into the
      // multisampled copy.
      Rc<IBackendImageView> shadowView;
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        if (IBackendImage* shadow = static_cast<D3D11Texture2D*>(pResource)->GetMsaaShadow()) {
          ImageViewDesc shadowDesc = viewDesc;
          shadowDesc.type = ImageViewType::View2DMS;
          shadowView = m_backendDevice.getBackend()->createImageView(shadow, shadowDesc);
        }
      }

      *ppDSView = new D3D11DepthStencilView(this, dsvDesc, pResource, std::move(backendView), std::move(shadowView));
      return S_OK;
    }

    // ---- ID3D11Device: Input Layout ----

    HRESULT STDMETHODCALLTYPE CreateInputLayout(
        const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
        UINT                            NumElements,
        const void*                     pShaderBytecodeWithInputSignature,
        SIZE_T                          BytecodeLength,
        ID3D11InputLayout**             ppInputLayout) override {
      // No elements make an empty layout, for shaders that read only
      // system values; the array is still required.
      if (!pInputElementDescs) return E_INVALIDARG;
      if (HRESULT hr = validateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature,
            BytecodeLength, formatSupportQuery()); FAILED(hr))
        return rejectInvalidCall("validateInputLayout", hr);
      if (!ppInputLayout) return S_FALSE;

      std::vector<D3D11_INPUT_ELEMENT_DESC> d3d11Elements(
          pInputElementDescs, pInputElementDescs + NumElements);

      std::vector<InputElementDesc> backendElements;
      backendElements.reserve(NumElements);

      for (UINT i = 0; i < NumElements; i++) {
        InputElementDesc e = {};
        e.semanticName      = pInputElementDescs[i].SemanticName;
        e.semanticIndex     = pInputElementDescs[i].SemanticIndex;
        e.format            = pInputElementDescs[i].Format;
        e.inputSlot         = pInputElementDescs[i].InputSlot;
        e.byteOffset        = pInputElementDescs[i].AlignedByteOffset;
        e.inputClass        = pInputElementDescs[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA
                              ? InputClassification::PerInstance
                              : InputClassification::PerVertex;
        e.instanceDataStepRate = pInputElementDescs[i].InstanceDataStepRate;
        backendElements.push_back(e);
      }

      *ppInputLayout = new D3D11InputLayout(this,
          std::move(d3d11Elements), std::move(backendElements));
      return S_OK;
    }

    // ---- ID3D11Device: Shader Creation ----

    HRESULT STDMETHODCALLTYPE CreateVertexShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11VertexShader**         ppVertexShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Vertex }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppVertexShader) return S_FALSE;

      logMessage(LogLevel::Trace, "CreateVertexShader(BytecodeLength=%zu)", BytecodeLength);
      auto shader = m_backendDevice.createVertexShader(pShaderBytecode, BytecodeLength);
      if (!shader) {
        logMessage(LogLevel::Error, "CreateVertexShader failed");
        return E_FAIL;
      }

      uint64_t shaderHash = hashShaderBytecode(pShaderBytecode, BytecodeLength);
      std::string bindingSummary = summarizeShaderBindings(pShaderBytecode, BytecodeLength);
      std::string inputSummary = summarizeVertexShaderInputs(pShaderBytecode, BytecodeLength);

      *ppVertexShader = new D3D11VertexShader(this, std::move(shader), shaderHash, std::move(bindingSummary), std::move(inputSummary));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreatePixelShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11PixelShader**          ppPixelShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Pixel }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppPixelShader) return S_FALSE;

      logMessage(LogLevel::Trace, "CreatePixelShader(BytecodeLength=%zu)", BytecodeLength);
      auto shader = m_backendDevice.createPixelShader(pShaderBytecode, BytecodeLength);
      if (!shader) {
        logMessage(LogLevel::Error, "CreatePixelShader failed");
        return E_FAIL;
      }

      uint64_t shaderHash = hashShaderBytecode(pShaderBytecode, BytecodeLength);
      std::string bindingSummary = summarizeShaderBindings(pShaderBytecode, BytecodeLength);

      *ppPixelShader = new D3D11PixelShader(this, std::move(shader), shaderHash, std::move(bindingSummary));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateGeometryShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11GeometryShader**       ppGeometryShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Geometry }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppGeometryShader) return S_FALSE;

      auto shader = m_backendDevice.createGeometryShader(pShaderBytecode, BytecodeLength);
      if (!shader) return E_FAIL;

      *ppGeometryShader = new D3D11GeometryShader(this, std::move(shader));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(
        const void*                     pShaderBytecode,
        SIZE_T                          BytecodeLength,
        const D3D11_SO_DECLARATION_ENTRY* pSODeclaration,
        UINT                            NumEntries,
        const UINT*                     pBufferStrides,
        UINT                            NumStrides,
        UINT                            RasterizedStream,
        ID3D11ClassLinkage*             pClassLinkage,
        ID3D11GeometryShader**          ppGeometryShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Geometry, DxbcProgramType::Vertex, DxbcProgramType::Domain }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppGeometryShader) return S_FALSE;

      auto shader = m_backendDevice.createGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength);
      if (!shader) return E_FAIL;

      *ppGeometryShader = new D3D11GeometryShader(this, std::move(shader),
        pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateHullShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11HullShader**           ppHullShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Hull }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppHullShader) return S_FALSE;

      auto shader = m_backendDevice.createHullShader(pShaderBytecode, BytecodeLength);
      if (!shader) return E_FAIL;

      *ppHullShader = new D3D11HullShader(this, std::move(shader));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDomainShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11DomainShader**         ppDomainShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Domain }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppDomainShader) return S_FALSE;

      auto shader = m_backendDevice.createDomainShader(pShaderBytecode, BytecodeLength);
      if (!shader) return E_FAIL;

      *ppDomainShader = new D3D11DomainShader(this, std::move(shader));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateComputeShader(
        const void*                  pShaderBytecode,
        SIZE_T                       BytecodeLength,
        ID3D11ClassLinkage*          pClassLinkage,
        ID3D11ComputeShader**        ppComputeShader) override {
      if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
      if (HRESULT hr = validateShaderBytecode(pShaderBytecode, BytecodeLength, { DxbcProgramType::Compute }); FAILED(hr))
        return rejectInvalidCall("validateShaderBytecode", hr);
      if (!ppComputeShader) return S_FALSE;

      auto shader = m_backendDevice.createComputeShader(pShaderBytecode, BytecodeLength);
      if (!shader) return E_FAIL;

      // Compile the PSO now instead of on the first Dispatch, which would
      // otherwise stall the render thread: on a compiler thread when there
      // are some, which the first Dispatch waits for if it is not done. The
      // context gets the pipeline on dispatch when there is none here.
      Rc<IBackendPipeline> pipeline;
      if (auto* backend = GetBackend()) {
        ComputePipelineDesc pipelineDesc = {};
        pipelineDesc.csBytecode = shader->getBytecode();
        pipelineDesc.csBytecodeSize = shader->getBytecodeSize();
        pipelineDesc.csBytecodeHash = shader->getBytecodeHash();
        if (backend->compilesInBackground())
          backend->requestComputePipeline(pipelineDesc);
        else
          pipeline = backend->createComputePipeline(pipelineDesc);
      }

      *ppComputeShader = new D3D11ComputeShader(this, std::move(shader), std::move(pipeline));
      return S_OK;
    }

    // ---- ID3D11Device: State Object Creation ----

    // The checks D3D11 applies to state descriptions, as the native
    // runtime answers them; D3D12 removes the device for some of them.
    static bool isValidComparisonFunc(D3D11_COMPARISON_FUNC func) {
      return func >= D3D11_COMPARISON_NEVER && func <= D3D11_COMPARISON_ALWAYS;
    }

    static HRESULT validateSamplerDesc(const D3D11_SAMPLER_DESC& desc) {
      const UINT filter = UINT(desc.Filter);
      const UINT reduction = filter & 0x180u;
      const UINT base = filter & ~0x180u;
      const bool validBase = base == 0x00 || base == 0x01 || base == 0x04 || base == 0x05 || base == 0x10
        || base == 0x11 || base == 0x14 || base == 0x15 || base == 0x55;
      auto validAddress = [](D3D11_TEXTURE_ADDRESS_MODE mode) {
        return mode >= D3D11_TEXTURE_ADDRESS_WRAP && mode <= D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
      };
      if ((filter & ~0x1FFu) || !validBase || desc.MaxAnisotropy > D3D11_MAX_MAXANISOTROPY
       || !validAddress(desc.AddressU) || !validAddress(desc.AddressV) || !validAddress(desc.AddressW)
       || !(desc.MipLODBias >= D3D11_MIP_LOD_BIAS_MIN && desc.MipLODBias <= D3D11_MIP_LOD_BIAS_MAX)
       || (reduction == 0x80u && !isValidComparisonFunc(desc.ComparisonFunc)))
        return E_INVALIDARG;
      return S_OK;
    }

    static HRESULT validateBlendDesc(const D3D11_BLEND_DESC& desc) {
      auto validFactor = [](D3D11_BLEND factor) {
        return factor >= D3D11_BLEND_ZERO && factor <= D3D11_BLEND_INV_SRC1_ALPHA
          && factor != 12 && factor != 13;
      };
      auto colorFactor = [](D3D11_BLEND factor) {
        return factor == D3D11_BLEND_SRC_COLOR || factor == D3D11_BLEND_INV_SRC_COLOR
          || factor == D3D11_BLEND_DEST_COLOR || factor == D3D11_BLEND_INV_DEST_COLOR
          || factor == D3D11_BLEND_SRC1_COLOR || factor == D3D11_BLEND_INV_SRC1_COLOR;
      };
      auto validOp = [](D3D11_BLEND_OP op) { return op >= D3D11_BLEND_OP_ADD && op <= D3D11_BLEND_OP_MAX; };
      const UINT targets = desc.IndependentBlendEnable ? 8u : 1u;
      for (UINT i = 0; i < targets; i++) {
        const auto& rt = desc.RenderTarget[i];
        if (rt.RenderTargetWriteMask & ~UINT8(D3D11_COLOR_WRITE_ENABLE_ALL))
          return E_INVALIDARG;
        if (!rt.BlendEnable)
          continue;
        if (!validFactor(rt.SrcBlend) || !validFactor(rt.DestBlend) || !validOp(rt.BlendOp)
         || !validFactor(rt.SrcBlendAlpha) || !validFactor(rt.DestBlendAlpha) || !validOp(rt.BlendOpAlpha)
         || colorFactor(rt.SrcBlendAlpha) || colorFactor(rt.DestBlendAlpha))
          return E_INVALIDARG;
      }
      return S_OK;
    }

    static HRESULT validateRasterizerDesc(const D3D11_RASTERIZER_DESC& desc) {
      if (desc.FillMode < D3D11_FILL_WIREFRAME || desc.FillMode > D3D11_FILL_SOLID
       || desc.CullMode < D3D11_CULL_NONE || desc.CullMode > D3D11_CULL_BACK
       || desc.DepthBiasClamp != desc.DepthBiasClamp || desc.SlopeScaledDepthBias != desc.SlopeScaledDepthBias)
        return E_INVALIDARG;
      return S_OK;
    }

    static HRESULT validateDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC& desc) {
      auto validOp = [](D3D11_STENCIL_OP op) { return op >= D3D11_STENCIL_OP_KEEP && op <= D3D11_STENCIL_OP_DECR; };
      auto validFace = [&](const D3D11_DEPTH_STENCILOP_DESC& face) {
        return validOp(face.StencilFailOp) && validOp(face.StencilDepthFailOp) && validOp(face.StencilPassOp)
          && isValidComparisonFunc(face.StencilFunc);
      };
      if (desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO && desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL)
        return E_INVALIDARG;
      if (desc.DepthEnable && !isValidComparisonFunc(desc.DepthFunc))
        return E_INVALIDARG;
      if (desc.StencilEnable && (!validFace(desc.FrontFace) || !validFace(desc.BackFace)))
        return E_INVALIDARG;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateBlendState(
        const D3D11_BLEND_DESC*   pBlendStateDesc,
        ID3D11BlendState**        ppBlendState) override {
      if (!pBlendStateDesc) return E_INVALIDARG;
      if (HRESULT hr = validateBlendDesc(*pBlendStateDesc); FAILED(hr)) return rejectInvalidCall("validateBlendDesc", hr);
      if (!ppBlendState) return S_FALSE;

      BlendState backendState = convertBlendDesc(pBlendStateDesc);
      *ppBlendState = new D3D11BlendState(this, *pBlendStateDesc, backendState);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(
        const D3D11_DEPTH_STENCIL_DESC*  pDepthStencilDesc,
        ID3D11DepthStencilState**        ppDepthStencilState) override {
      if (!pDepthStencilDesc) return E_INVALIDARG;
      if (HRESULT hr = validateDepthStencilDesc(*pDepthStencilDesc); FAILED(hr)) return rejectInvalidCall("validateDepthStencilDesc", hr);
      if (!ppDepthStencilState) return S_FALSE;

      DepthStencilState backendState = convertDepthStencilDesc(pDepthStencilDesc);
      *ppDepthStencilState = new D3D11DepthStencilState(this, *pDepthStencilDesc, backendState);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRasterizerState(
        const D3D11_RASTERIZER_DESC*  pRasterizerDesc,
        ID3D11RasterizerState**       ppRasterizerState) override {
      if (!pRasterizerDesc) return E_INVALIDARG;
      if (HRESULT hr = validateRasterizerDesc(*pRasterizerDesc); FAILED(hr)) return rejectInvalidCall("validateRasterizerDesc", hr);
      if (!ppRasterizerState) return S_FALSE;

      RasterizerState backendState = convertRasterizerDesc(pRasterizerDesc);
      *ppRasterizerState = new D3D11RasterizerState(this, *pRasterizerDesc, backendState);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateSamplerState(
        const D3D11_SAMPLER_DESC*  pSamplerDesc,
        ID3D11SamplerState**       ppSamplerState) override {
      if (!pSamplerDesc) return E_INVALIDARG;
      if (HRESULT hr = validateSamplerDesc(*pSamplerDesc); FAILED(hr)) return rejectInvalidCall("validateSamplerDesc", hr);
      if (!ppSamplerState) return S_FALSE;

      // As in D3D11, an identical description returns the same object.
      const std::string key(reinterpret_cast<const char*>(pSamplerDesc), sizeof(*pSamplerDesc));
      std::lock_guard<std::mutex> lock(m_samplerCacheMutex);
      auto cached = m_samplerCache.find(key);
      if (cached != m_samplerCache.end() && cached->second->tryAddRefFromCache()) {
        *ppSamplerState = cached->second;
        return S_OK;
      }

      auto backendSampler = m_backendDevice.createSampler(pSamplerDesc);
      if (!backendSampler) return E_FAIL;

      auto* sampler = new D3D11SamplerState(this, *pSamplerDesc, std::move(backendSampler));
      m_samplerCache[key] = sampler;
      *ppSamplerState = sampler;
      return S_OK;
    }

    void forgetSamplerState(const D3D11_SAMPLER_DESC& desc, D3D11SamplerState* sampler) {
      const std::string key(reinterpret_cast<const char*>(&desc), sizeof(desc));
      std::lock_guard<std::mutex> lock(m_samplerCacheMutex);
      auto it = m_samplerCache.find(key);
      if (it != m_samplerCache.end() && it->second == sampler)
        m_samplerCache.erase(it);
    }

    static bool translateQueryDesc(const D3D11_QUERY_DESC& d3dDesc, QueryDesc& desc) {
      desc = QueryDesc();
      switch (d3dDesc.Query) {
        case D3D11_QUERY_EVENT:
          desc.type = QueryType::Event;
          return true;
        case D3D11_QUERY_OCCLUSION:
          desc.type = QueryType::Occlusion;
          return true;
        case D3D11_QUERY_OCCLUSION_PREDICATE:
          desc.type = QueryType::Occlusion;
          desc.binary = true;
          return true;
        case D3D11_QUERY_TIMESTAMP:
          desc.type = QueryType::Timestamp;
          return true;
        case D3D11_QUERY_TIMESTAMP_DISJOINT:
          desc.type = QueryType::TimestampDisjoint;
          return true;
        case D3D11_QUERY_PIPELINE_STATISTICS:
          desc.type = QueryType::PipelineStatistics;
          return true;
        case D3D11_QUERY_SO_STATISTICS:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
          desc.type = QueryType::StreamOutputStatistics;
          desc.stream = QueryDesc::kAllStreams;
          return true;
        case D3D11_QUERY_SO_STATISTICS_STREAM0:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
          desc.type = QueryType::StreamOutputStatistics;
          desc.stream = (d3dDesc.Query - D3D11_QUERY_SO_STATISTICS_STREAM0) / 2;
          return true;
        default:
          return false;
      }
    }

    static bool isPredicateQuery(D3D11_QUERY query) {
      return query == D3D11_QUERY_OCCLUSION_PREDICATE
          || query == D3D11_QUERY_SO_OVERFLOW_PREDICATE
          || query == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0
          || query == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1
          || query == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2
          || query == D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3;
    }

    HRESULT STDMETHODCALLTYPE CreateQuery(
        const D3D11_QUERY_DESC*  pQueryDesc,
        ID3D11Query**            ppQuery) override {
      if (!pQueryDesc) return E_INVALIDARG;

      QueryDesc desc;
      if (!translateQueryDesc(*pQueryDesc, desc))
        return E_INVALIDARG;
      if (!ppQuery) return S_FALSE;

      auto backendQuery = m_backendDevice.getBackend()->createQuery(desc);
      if (!backendQuery) return E_FAIL;

      *ppQuery = new D3D11Query(this, *pQueryDesc, std::move(backendQuery));
      return S_OK;
    }

    // ---- ID3D11Device: Predicate / Counter / Class Linkage ----

    HRESULT STDMETHODCALLTYPE CreatePredicate(
        const D3D11_QUERY_DESC*  pPredicateDesc,
        ID3D11Predicate**        ppPredicate) override {
      if (!pPredicateDesc) return E_INVALIDARG;

      QueryDesc desc;
      if (!isPredicateQuery(pPredicateDesc->Query) || !translateQueryDesc(*pPredicateDesc, desc))
        return E_INVALIDARG;
      if (!ppPredicate) return S_FALSE;

      auto backendQuery = m_backendDevice.getBackend()->createQuery(desc);
      if (!backendQuery) return E_FAIL;
      *ppPredicate = reinterpret_cast<ID3D11Predicate*>(
        new D3D11Query(this, *pPredicateDesc, std::move(backendQuery)));
      return S_OK;
    }

    // CheckCounterInfo reports no device-dependent counters, and the
    // D3D11 counter types are optional.
    HRESULT STDMETHODCALLTYPE CreateCounter(
        const D3D11_COUNTER_DESC* pCounterDesc,
        ID3D11Counter**           ppCounter) override {
      if (!pCounterDesc) return E_INVALIDARG;
      if (ppCounter) *ppCounter = nullptr;
      return DXGI_ERROR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE CreateClassLinkage(
        ID3D11ClassLinkage** ppLinkage) override {
      if (!ppLinkage) return E_INVALIDARG;
      *ppLinkage = new D3D11ClassLinkage(this);
      return S_OK;
    }

    // ---- ID3D11Device: Deferred Context ----

    HRESULT STDMETHODCALLTYPE CreateDeferredContext(
        UINT                        ContextFlags,
        ID3D11DeviceContext**       ppDeferredContext) override;  // Defined after D3D11ImmediateContext

    // ---- ID3D11Device: Immediate Context ----

    // Implemented after D3D11ImmediateContext is defined (see d3d11_context.h)
    void STDMETHODCALLTYPE GetImmediateContext(
        ID3D11DeviceContext** ppImmediateContext) override;

    void STDMETHODCALLTYPE GetImmediateContext1(
        ID3D11DeviceContext1** ppImmediateContext) override {
      GetImmediateContext(reinterpret_cast<ID3D11DeviceContext**>(ppImmediateContext));
    }

    HRESULT STDMETHODCALLTYPE CreateDeferredContext1(
        UINT                   ContextFlags,
        ID3D11DeviceContext1** ppDeferredContext) override {
      return CreateDeferredContext(ContextFlags, reinterpret_cast<ID3D11DeviceContext**>(ppDeferredContext));
    }

    HRESULT STDMETHODCALLTYPE CreateBlendState1(
        const D3D11_BLEND_DESC1* pBlendStateDesc,
        ID3D11BlendState1**      ppBlendState) override {
      if (!pBlendStateDesc) return E_INVALIDARG;
      D3D11_BLEND_DESC baseDesc = downgradeBlendDesc1(pBlendStateDesc);
      if (HRESULT hr = validateBlendDesc(baseDesc); FAILED(hr)) return rejectInvalidCall("validateBlendDesc", hr);
      for (UINT i = 0; i < (pBlendStateDesc->IndependentBlendEnable ? 8u : 1u); i++) {
        const auto& rt = pBlendStateDesc->RenderTarget[i];
        if (rt.LogicOpEnable && (rt.BlendEnable || rt.LogicOp > D3D11_LOGIC_OP_OR_INVERTED))
          return E_INVALIDARG;
      }
      if (!ppBlendState) return S_FALSE;

      BlendState backendState = convertBlendDesc(&baseDesc);
      for (uint32_t i = 0; i < 8; i++) {
        backendState.renderTargets[i].logicOpEnable = pBlendStateDesc->RenderTarget[i].LogicOpEnable == TRUE;
        backendState.renderTargets[i].logicOp = static_cast<uint8_t>(pBlendStateDesc->RenderTarget[i].LogicOp);
      }
      *ppBlendState = new D3D11BlendState(this, *pBlendStateDesc, backendState);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRasterizerState1(
        const D3D11_RASTERIZER_DESC1* pRasterizerDesc,
        ID3D11RasterizerState1**      ppRasterizerState) override {
      if (!pRasterizerDesc) return E_INVALIDARG;
      D3D11_RASTERIZER_DESC baseDesc = downgradeRasterizerDesc1(pRasterizerDesc);
      if (HRESULT hr = validateRasterizerDesc(baseDesc); FAILED(hr)) return rejectInvalidCall("validateRasterizerDesc", hr);
      if (!ppRasterizerState) return S_FALSE;

      RasterizerState backendState = convertRasterizerDesc(&baseDesc);
      backendState.forcedSampleCount = pRasterizerDesc->ForcedSampleCount;
      *ppRasterizerState = new D3D11RasterizerState(this, *pRasterizerDesc, backendState);
      return S_OK;
    }

    // ---- ID3D11Device2 ----

    // Defined in d3d11_context.h with the context.
    void STDMETHODCALLTYPE GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext) override;

    // Tiled resources are reported unsupported.
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D11Resource*           pTiledResource,
        UINT*                     pNumTilesForEntireResource,
        D3D11_PACKED_MIP_DESC*    pPackedMipDesc,
        D3D11_TILE_SHAPE*         pStandardTileShapeForNonPackedMips,
        UINT*                     pNumSubresourceTilings,
        UINT                      FirstSubresourceTilingToGet,
        D3D11_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override {
      // The D3D11 structures have the D3D12 layout.
      static_assert(sizeof(D3D11_PACKED_MIP_DESC) == sizeof(D3D12_PACKED_MIP_INFO), "packed mip layout");
      static_assert(sizeof(D3D11_TILE_SHAPE) == sizeof(D3D12_TILE_SHAPE), "tile shape layout");
      static_assert(sizeof(D3D11_SUBRESOURCE_TILING) == sizeof(D3D12_SUBRESOURCE_TILING), "subresource tiling layout");
      ID3D12Device* device = nativeDevice();
      ID3D12Resource* resource = nativeResource(pTiledResource);
      if (!device || !resource) {
        if (pNumTilesForEntireResource) *pNumTilesForEntireResource = 0;
        if (pPackedMipDesc) *pPackedMipDesc = {};
        if (pStandardTileShapeForNonPackedMips) *pStandardTileShapeForNonPackedMips = {};
        if (pNumSubresourceTilings) *pNumSubresourceTilings = 0;
        return;
      }
      device->GetResourceTiling(resource, pNumTilesForEntireResource,
        reinterpret_cast<D3D12_PACKED_MIP_INFO*>(pPackedMipDesc),
        reinterpret_cast<D3D12_TILE_SHAPE*>(pStandardTileShapeForNonPackedMips),
        pNumSubresourceTilings, FirstSubresourceTilingToGet,
        reinterpret_cast<D3D12_SUBRESOURCE_TILING*>(pSubresourceTilingsForNonPackedMips));
    }

    // The D3D12 resource behind a D3D11 buffer or texture.
    ID3D12Resource* nativeResource(ID3D11Resource* resource) {
      if (!resource)
        return nullptr;
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
          auto* buffer = static_cast<D3D11Buffer*>(static_cast<ID3D11Buffer*>(resource))->GetBackendBuffer();
          return buffer ? static_cast<d3d12::D3D12BackendBuffer*>(buffer)->getResource() : nullptr;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          auto* image = static_cast<D3D11Texture1D*>(static_cast<ID3D11Texture1D*>(resource))->GetBackendImage();
          return image ? static_cast<d3d12::D3D12BackendImage*>(image)->getResource() : nullptr;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          auto* image = static_cast<D3D11Texture2D*>(static_cast<ID3D11Texture2D*>(resource))->GetBackendImage();
          return image ? static_cast<d3d12::D3D12BackendImage*>(image)->getResource() : nullptr;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          auto* image = static_cast<D3D11Texture3D*>(static_cast<ID3D11Texture3D*>(resource))->GetBackendImage();
          return image ? static_cast<d3d12::D3D12BackendImage*>(image)->getResource() : nullptr;
        }
        default:
          return nullptr;
      }
    }

    // D3D11 has tiers 1 to 3, numbered like D3D12's.
    D3D11_TILED_RESOURCES_TIER tiledResourcesTier() {
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      ID3D12Device* device = nativeDevice();
      if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        return D3D11_TILED_RESOURCES_NOT_SUPPORTED;
      const UINT tier = static_cast<UINT>(options.TiledResourcesTier);
      return static_cast<D3D11_TILED_RESOURCES_TIER>(tier > 3 ? 3 : tier);
    }

    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels1(
        DXGI_FORMAT Format,
        UINT        SampleCount,
        UINT        Flags,
        UINT*       pNumQualityLevels) override {
      if (!pNumQualityLevels) return E_INVALIDARG;
      if (Flags & D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE) {
        *pNumQualityLevels = 0;
        ID3D12Device* device = nativeDevice();
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format = Format;
        levels.SampleCount = SampleCount;
        levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE;
        if (device && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))))
          *pNumQualityLevels = levels.NumQualityLevels;
        return S_OK;
      }
      return CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
    }

    // ---- ID3D11Device3 ----

    HRESULT STDMETHODCALLTYPE CreateTexture2D1(
        const D3D11_TEXTURE2D_DESC1*  pDesc1,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture2D1**            ppTexture2D) override {
      if (!pDesc1) return E_INVALIDARG;
      // Only the driver-chosen layout exists without map-on-default support.
      if (pDesc1->TextureLayout != D3D11_TEXTURE_LAYOUT_UNDEFINED) return E_INVALIDARG;
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width          = pDesc1->Width;
      desc.Height         = pDesc1->Height;
      desc.MipLevels      = pDesc1->MipLevels;
      desc.ArraySize      = pDesc1->ArraySize;
      desc.Format         = pDesc1->Format;
      desc.SampleDesc     = pDesc1->SampleDesc;
      desc.Usage          = pDesc1->Usage;
      desc.BindFlags      = pDesc1->BindFlags;
      desc.CPUAccessFlags = pDesc1->CPUAccessFlags;
      desc.MiscFlags      = pDesc1->MiscFlags;
      return CreateTexture2D(&desc, pInitialData, reinterpret_cast<ID3D11Texture2D**>(ppTexture2D));
    }

    HRESULT STDMETHODCALLTYPE CreateTexture3D1(
        const D3D11_TEXTURE3D_DESC1*  pDesc1,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture3D1**            ppTexture3D) override {
      if (!pDesc1) return E_INVALIDARG;
      if (pDesc1->TextureLayout != D3D11_TEXTURE_LAYOUT_UNDEFINED) return E_INVALIDARG;
      D3D11_TEXTURE3D_DESC desc = {};
      desc.Width          = pDesc1->Width;
      desc.Height         = pDesc1->Height;
      desc.Depth          = pDesc1->Depth;
      desc.MipLevels      = pDesc1->MipLevels;
      desc.Format         = pDesc1->Format;
      desc.Usage          = pDesc1->Usage;
      desc.BindFlags      = pDesc1->BindFlags;
      desc.CPUAccessFlags = pDesc1->CPUAccessFlags;
      desc.MiscFlags      = pDesc1->MiscFlags;
      return CreateTexture3D(&desc, pInitialData, reinterpret_cast<ID3D11Texture3D**>(ppTexture3D));
    }

    HRESULT STDMETHODCALLTYPE CreateRasterizerState2(
        const D3D11_RASTERIZER_DESC2* pRasterizerDesc,
        ID3D11RasterizerState2**      ppRasterizerState) override {
      if (!pRasterizerDesc) return E_INVALIDARG;
      const bool conservative = pRasterizerDesc->ConservativeRaster != D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
      if (conservative && conservativeRasterizationTier() == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED)
        return E_INVALIDARG;
      D3D11_RASTERIZER_DESC1 desc1;
      std::memcpy(&desc1, pRasterizerDesc, sizeof(desc1));
      D3D11_RASTERIZER_DESC baseDesc = downgradeRasterizerDesc1(&desc1);
      if (HRESULT hr = validateRasterizerDesc(baseDesc); FAILED(hr)) return rejectInvalidCall("validateRasterizerDesc", hr);
      if (!ppRasterizerState) return S_FALSE;

      RasterizerState backendState = convertRasterizerDesc(&baseDesc);
      backendState.forcedSampleCount = desc1.ForcedSampleCount;
      backendState.conservativeRaster = conservative;
      *ppRasterizerState = new D3D11RasterizerState(this, *pRasterizerDesc, backendState);
      return S_OK;
    }

    // Planar formats are not supported, so plane slices other than 0 are
    // invalid; the rest of the descriptions match.
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(
        ID3D11Resource*                          pResource,
        const D3D11_SHADER_RESOURCE_VIEW_DESC1*  pDesc1,
        ID3D11ShaderResourceView1**              ppSRView1) override {
      if (!pDesc1)
        return CreateShaderResourceView(pResource, nullptr, reinterpret_cast<ID3D11ShaderResourceView**>(ppSRView1));
      D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
      desc.Format        = pDesc1->Format;
      desc.ViewDimension = pDesc1->ViewDimension;
      switch (pDesc1->ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
          if (pDesc1->Texture2D.PlaneSlice) return E_INVALIDARG;
          desc.Texture2D.MostDetailedMip = pDesc1->Texture2D.MostDetailedMip;
          desc.Texture2D.MipLevels       = pDesc1->Texture2D.MipLevels;
          break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
          if (pDesc1->Texture2DArray.PlaneSlice) return E_INVALIDARG;
          desc.Texture2DArray.MostDetailedMip = pDesc1->Texture2DArray.MostDetailedMip;
          desc.Texture2DArray.MipLevels       = pDesc1->Texture2DArray.MipLevels;
          desc.Texture2DArray.FirstArraySlice = pDesc1->Texture2DArray.FirstArraySlice;
          desc.Texture2DArray.ArraySize       = pDesc1->Texture2DArray.ArraySize;
          break;
        default:
          std::memcpy(&desc.Buffer, &pDesc1->Buffer, sizeof(desc) - offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Buffer));
          break;
      }
      return CreateShaderResourceView(pResource, &desc, reinterpret_cast<ID3D11ShaderResourceView**>(ppSRView1));
    }

    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(
        ID3D11Resource*                           pResource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC1*  pDesc1,
        ID3D11UnorderedAccessView1**              ppUAView1) override {
      if (!pDesc1)
        return CreateUnorderedAccessView(pResource, nullptr, reinterpret_cast<ID3D11UnorderedAccessView**>(ppUAView1));
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
      desc.Format        = pDesc1->Format;
      desc.ViewDimension = pDesc1->ViewDimension;
      switch (pDesc1->ViewDimension) {
        case D3D11_UAV_DIMENSION_TEXTURE2D:
          if (pDesc1->Texture2D.PlaneSlice) return E_INVALIDARG;
          desc.Texture2D.MipSlice = pDesc1->Texture2D.MipSlice;
          break;
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          if (pDesc1->Texture2DArray.PlaneSlice) return E_INVALIDARG;
          desc.Texture2DArray.MipSlice        = pDesc1->Texture2DArray.MipSlice;
          desc.Texture2DArray.FirstArraySlice = pDesc1->Texture2DArray.FirstArraySlice;
          desc.Texture2DArray.ArraySize       = pDesc1->Texture2DArray.ArraySize;
          break;
        default:
          std::memcpy(&desc.Buffer, &pDesc1->Buffer, sizeof(desc) - offsetof(D3D11_UNORDERED_ACCESS_VIEW_DESC, Buffer));
          break;
      }
      return CreateUnorderedAccessView(pResource, &desc, reinterpret_cast<ID3D11UnorderedAccessView**>(ppUAView1));
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(
        ID3D11Resource*                        pResource,
        const D3D11_RENDER_TARGET_VIEW_DESC1*  pDesc1,
        ID3D11RenderTargetView1**              ppRTView1) override {
      if (!pDesc1)
        return CreateRenderTargetView(pResource, nullptr, reinterpret_cast<ID3D11RenderTargetView**>(ppRTView1));
      D3D11_RENDER_TARGET_VIEW_DESC desc = {};
      desc.Format        = pDesc1->Format;
      desc.ViewDimension = pDesc1->ViewDimension;
      switch (pDesc1->ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE2D:
          if (pDesc1->Texture2D.PlaneSlice) return E_INVALIDARG;
          desc.Texture2D.MipSlice = pDesc1->Texture2D.MipSlice;
          break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
          if (pDesc1->Texture2DArray.PlaneSlice) return E_INVALIDARG;
          desc.Texture2DArray.MipSlice        = pDesc1->Texture2DArray.MipSlice;
          desc.Texture2DArray.FirstArraySlice = pDesc1->Texture2DArray.FirstArraySlice;
          desc.Texture2DArray.ArraySize       = pDesc1->Texture2DArray.ArraySize;
          break;
        default:
          std::memcpy(&desc.Buffer, &pDesc1->Buffer, sizeof(desc) - offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Buffer));
          break;
      }
      return CreateRenderTargetView(pResource, &desc, reinterpret_cast<ID3D11RenderTargetView**>(ppRTView1));
    }

    HRESULT STDMETHODCALLTYPE CreateQuery1(
        const D3D11_QUERY_DESC1* pQueryDesc1,
        ID3D11Query1**           ppQuery1) override {
      if (!pQueryDesc1) return E_INVALIDARG;
      // Queries count work of every kind on the single queue.
      D3D11_QUERY_DESC desc = { pQueryDesc1->Query, pQueryDesc1->MiscFlags };
      return CreateQuery(&desc, reinterpret_cast<ID3D11Query**>(ppQuery1));
    }

    void STDMETHODCALLTYPE GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext3(UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext) override;

    // Textures cannot be mapped on the default heap (MapOnDefaultTextures is
    // reported unsupported), so there is nothing these can access.
    void STDMETHODCALLTYPE WriteToSubresource(
        ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
        const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override {
      (void)pDstResource; (void)DstSubresource; (void)pDstBox;
      (void)pSrcData; (void)SrcRowPitch; (void)SrcDepthPitch;
    }

    void STDMETHODCALLTYPE ReadFromSubresource(
        void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
        ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override {
      (void)pDstData; (void)DstRowPitch; (void)DstDepthPitch;
      (void)pSrcResource; (void)SrcSubresource; (void)pSrcBox;
    }

    // ---- ID3D11Device4 ----

    // A D3D12 fence completes with UINT64_MAX when the device is removed.
    HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE hEvent, DWORD* pdwCookie) override {
      ID3D12Device* device = nativeDevice();
      if (!hEvent || !pdwCookie || !device) return E_INVALIDARG;
      ComPtr<ID3D12Fence> fence;
      HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
      if (FAILED(hr)) return hr;
      hr = fence->SetEventOnCompletion(UINT64_MAX, hEvent);
      if (FAILED(hr)) return hr;
      std::lock_guard<std::mutex> lock(m_removedEventMutex);
      *pdwCookie = ++m_removedEventCookie;
      m_removedEventFences.push_back({ *pdwCookie, std::move(fence) });
      return S_OK;
    }

    void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD dwCookie) override {
      std::lock_guard<std::mutex> lock(m_removedEventMutex);
      for (size_t i = 0; i < m_removedEventFences.size(); i++) {
        if (m_removedEventFences[i].first == dwCookie) {
          m_removedEventFences.erase(m_removedEventFences.begin() + i);
          return;
        }
      }
    }

    // ---- ID3D11Device5 ----

    HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE hFence, REFIID ReturnedInterface, void** ppFence) override {
      ID3D12Device* device = nativeDevice();
      if (!ppFence) return E_INVALIDARG;
      *ppFence = nullptr;
      if (!device) return E_FAIL;
      ComPtr<ID3D12Fence> fence;
      HRESULT hr = device->OpenSharedHandle(hFence, IID_PPV_ARGS(&fence));
      if (FAILED(hr)) return hr;
      auto* wrapper = new D3D11Fence(this, std::move(fence));
      hr = wrapper->QueryInterface(ReturnedInterface, ppFence);
      wrapper->Release();
      return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D11_FENCE_FLAG Flags,
                                          REFIID ReturnedInterface, void** ppFence) override {
      ID3D12Device* device = nativeDevice();
      if (!ppFence) return E_INVALIDARG;
      *ppFence = nullptr;
      if (!device) return E_FAIL;
      // D3D11 and D3D12 fence flags have the same values.
      ComPtr<ID3D12Fence> fence;
      HRESULT hr = device->CreateFence(InitialValue, static_cast<D3D12_FENCE_FLAGS>(Flags), IID_PPV_ARGS(&fence));
      if (FAILED(hr)) return hr;
      auto* wrapper = new D3D11Fence(this, std::move(fence));
      hr = wrapper->QueryInterface(ReturnedInterface, ppFence);
      wrapper->Release();
      return hr;
    }

    // Defined in d3d11_context.h, which knows the context state.
    HRESULT STDMETHODCALLTYPE CreateDeviceContextState(
        UINT                     Flags,
        const D3D_FEATURE_LEVEL* pFeatureLevels,
        UINT                     FeatureLevels,
        UINT                     SDKVersion,
        REFIID                   EmulatedInterface,
        D3D_FEATURE_LEVEL*       pChosenFeatureLevel,
        ID3DDeviceContextState** ppContextState) override;

    // NT handles, as CreateSharedHandle makes them.
    HRESULT STDMETHODCALLTYPE OpenSharedResource1(
        HANDLE hResource,
        REFIID returnedInterface,
        void** ppResource) override {
      if (ppResource)
        *ppResource = nullptr;
      if (!D3D11SharedResource::isNtHandle(hResource) || isEmulatedKmtHandle(hResource))
        return E_INVALIDARG;
      return openSharedResource(hResource, true, returnedInterface, ppResource);
    }

    HRESULT STDMETHODCALLTYPE OpenSharedResourceByName(
        LPCWSTR lpName,
        DWORD   dwDesiredAccess,
        REFIID  returnedInterface,
        void**  ppResource) override {
      (void)dwDesiredAccess;
      if (ppResource)
        *ppResource = nullptr;
      ID3D12Device* device = nativeDevice();
      HANDLE handle = nullptr;
      if (!lpName || !device || FAILED(device->OpenSharedHandleByName(lpName, GENERIC_ALL, &handle)))
        return E_INVALIDARG;
      const HRESULT hr = openSharedResource(handle, true, returnedInterface, ppResource);
      CloseHandle(handle);
      return hr;
    }

    // ---- ID3D11Device: Misc ----

    // Legacy handles, of other runtimes too; the ones DXUP hands out are NT
    // handles of this process.
    HRESULT STDMETHODCALLTYPE OpenSharedResource(
        HANDLE      hResource,
        REFIID      ReturnedInterface,
        void**      ppResource) override {
      if (ppResource)
        *ppResource = nullptr;
      if (D3D11SharedResource::isNtHandle(hResource) && !isEmulatedKmtHandle(hResource))
        return E_INVALIDARG;
      return openSharedResource(hResource, false, ReturnedInterface, ppResource);
    }

    // The legacy handles DXUP hands out are NT handles of this process.
    static bool isEmulatedKmtHandle(HANDLE handle) {
      auto shared = D3D11SharedResourceRegistry::find(handle);
      return shared && shared->isKmtHandle(handle);
    }

    // Resources created shared: what the devices that open them report,
    // and the order of their work on them.
    void makeShared(D3D11DxgiResource& dxgi, D3D11_RESOURCE_DIMENSION dimension, const void* desc, size_t descSize,
                    std::shared_ptr<d3d12::D3D12SharedSync> sync) {
      if (!sync) {
        // Staging resources live in CPU-visible heaps, which D3D12 does not share.
        logMessage(LogLevel::Warn, "shared resource of dimension %u is not shareable (staging)", unsigned(dimension));
        return;
      }
      auto shared = std::make_shared<D3D11SharedResource>();
      shared->dimension = dimension;
      std::memcpy(&shared->desc, desc, descSize);
      shared->sync = std::move(sync);
      D3D11SharedResourceRegistry::add(shared);
      dxgi.setShared(std::move(shared));
    }

    HRESULT openSharedResource(HANDLE handle, bool ntHandle, REFIID riid, void** ppResource) {
      if (!ppResource)
        return E_INVALIDARG;
      *ppResource = nullptr;
      ID3D12Device* device = nativeDevice();
      ComPtr<ID3D12Resource> resource;
      if (!handle || !device || FAILED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&resource))))
        return E_INVALIDARG;

      // Resources of this process report their description; others one
      // derived from the D3D12 resource.
      auto shared = D3D11SharedResourceRegistry::find(handle);
      logMessage(LogLevel::Info, "OpenSharedResource: handle=%p nt=%d %s", handle, ntHandle ? 1 : 0,
        shared ? "of this process" : "of another runtime");
      if (!shared) {
        shared = describeSharedResource(resource->GetDesc(), ntHandle);
        if (!shared)
          return E_INVALIDARG;
        if (D3D11SharedResource::isNtHandle(handle))
          shared->addHandle(handle);
        else
          shared->setForeignKmtHandle(handle);
        D3D11SharedResourceRegistry::add(shared);
      }

      ID3D11Resource* wrapper = wrapSharedResource(std::move(resource), shared);
      if (!wrapper)
        return E_FAIL;
      const HRESULT hr = wrapper->QueryInterface(riid, ppResource);
      wrapper->Release();
      return hr;
    }

    static std::shared_ptr<D3D11SharedResource> describeSharedResource(const D3D12_RESOURCE_DESC& rd, bool ntHandle) {
      auto shared = std::make_shared<D3D11SharedResource>();
      shared->sync = std::make_shared<d3d12::D3D12SharedSync>();
      const UINT misc = D3D11_RESOURCE_MISC_SHARED | (ntHandle ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0u);
      const UINT bind = getWrappedResourceBindFlags(rd, nullptr);
      switch (rd.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:
          shared->dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
          shared->desc.buffer = { UINT(rd.Width), D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_SHARED, 0 };
          return shared;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
          shared->dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
          shared->desc.texture1D = { UINT(rd.Width), rd.MipLevels, rd.DepthOrArraySize, rd.Format,
            D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_SHARED };
          return shared;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
          shared->dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
          shared->desc.texture2D = { UINT(rd.Width), rd.Height, rd.MipLevels, rd.DepthOrArraySize, rd.Format,
            rd.SampleDesc, D3D11_USAGE_DEFAULT, bind, 0, misc };
          return shared;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
          shared->dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
          shared->desc.texture3D = { UINT(rd.Width), rd.Height, rd.DepthOrArraySize, rd.MipLevels, rd.Format,
            D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_SHARED };
          return shared;
        default:
          return nullptr;
      }
    }

    // A D3D11 resource for an opened D3D12 resource, which starts in COMMON
    // as other devices leave it.
    ID3D11Resource* wrapSharedResource(ComPtr<ID3D12Resource> resource, const std::shared_ptr<D3D11SharedResource>& shared) {
      auto* backend = m_backendDevice.getBackend();
      if (!backend)
        return nullptr;
      const bool simultaneous = (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
      auto makeImage = [&](ImageType type, UINT width, UINT height, UINT depth, UINT mips, UINT array, DXGI_FORMAT format,
                           DXGI_SAMPLE_DESC samples, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc) {
        ImageDesc desc = {};
        desc.type = type;
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        desc.mipLevels = mips;
        desc.arraySize = array;
        desc.format = format;
        desc.sampleCount = samples.Count;
        desc.sampleQuality = samples.Quality;
        desc.usage = convertUsage(usage);
        desc.bindFlags = convertBindFlags(bind);
        desc.cpuAccess = convertCpuAccess(cpu);
        desc.miscFlags = misc;
        auto image = std::make_shared<d3d12::D3D12BackendImage>(std::move(resource), desc, simultaneous);
        image->setCurrentState(D3D12_RESOURCE_STATE_COMMON);
        image->setSharedSync(shared->sync);
        return BackendRetireQueue::wrap<IBackendImage>(backend->getRetireQueue(), std::move(image));
      };

      switch (shared->dimension) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
          const auto& d = shared->desc.buffer;
          BufferDesc desc = {};
          desc.size = d.ByteWidth;
          desc.structStride = d.StructureByteStride;
          desc.usage = convertUsage(d.Usage);
          desc.bindFlags = convertBindFlags(d.BindFlags);
          desc.cpuAccess = convertCpuAccess(d.CPUAccessFlags);
          desc.miscFlags = d.MiscFlags;
          auto buffer = std::make_shared<d3d12::D3D12BackendBuffer>(std::move(resource), desc);
          buffer->setCurrentState(D3D12_RESOURCE_STATE_COMMON);
          buffer->setSharedSync(shared->sync);
          auto* wrapper = new D3D11Buffer(this, d, BackendRetireQueue::wrap<IBackendBuffer>(backend->getRetireQueue(), std::move(buffer)));
          wrapper->dxgiResource().setShared(shared);
          return wrapper;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          const auto& d = shared->desc.texture1D;
          auto* wrapper = new D3D11Texture1D(this, d, makeImage(ImageType::Tex1D, d.Width, 1, 1, d.MipLevels, d.ArraySize,
            d.Format, DXGI_SAMPLE_DESC{ 1, 0 }, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags));
          wrapper->dxgiResource().setShared(shared);
          return wrapper;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          const auto& d = shared->desc.texture2D;
          auto* wrapper = new D3D11Texture2D(this, d, makeImage(ImageType::Tex2D, d.Width, d.Height, 1, d.MipLevels, d.ArraySize,
            d.Format, d.SampleDesc, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags));
          wrapper->dxgiResource().setShared(shared);
          return wrapper;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          const auto& d = shared->desc.texture3D;
          auto* wrapper = new D3D11Texture3D(this, d, makeImage(ImageType::Tex3D, d.Width, d.Height, d.Depth, d.MipLevels, 1,
            d.Format, DXGI_SAMPLE_DESC{ 1, 0 }, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags));
          wrapper->dxgiResource().setShared(shared);
          return wrapper;
        }
        default:
          return nullptr;
      }
    }

    // Submits the immediate context's work, then runs fn in order with it.
    // Defined in d3d11_context.h.
    void submitThen(std::function<void()> fn);

    // Other devices see what this device wrote to a shared resource once
    // Flush returns: while it holds any, Flush waits for the submission.
    void sharedResourceAdded() { m_sharedResources.fetch_add(1, std::memory_order_relaxed); }
    void sharedResourceRemoved() { m_sharedResources.fetch_sub(1, std::memory_order_relaxed); }
    bool hasSharedResources() const { return m_sharedResources.load(std::memory_order_relaxed) != 0; }

    // CheckFormatSupport for view validation, remembered per format.
    UINT getFormatSupport(DXGI_FORMAT format) {
      const UINT index = UINT(format);
      if (index < m_formatSupportCache.size()) {
        const uint64_t cached = m_formatSupportCache[index].load(std::memory_order_relaxed);
        if (cached >> 32)
          return UINT(cached);
      }
      UINT support = 0;
      if (FAILED(CheckFormatSupport(format, &support)))
        support = 0;
      if (index < m_formatSupportCache.size())
        m_formatSupportCache[index].store((uint64_t(1) << 32) | support, std::memory_order_relaxed);
      return support;
    }

    FormatSupportQuery formatSupportQuery() {
      return [this](DXGI_FORMAT format) { return getFormatSupport(format); };
    }

    HRESULT STDMETHODCALLTYPE CheckFormatSupport(
        DXGI_FORMAT Format,
        UINT*       pFormatSupport) override {
      if (!pFormatSupport) return E_INVALIDARG;
      // The feature query's answer; formats without support fail.
      D3D11_FEATURE_DATA_FORMAT_SUPPORT support = { Format };
      *pFormatSupport = 0;
      if (FAILED(CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
       || !support.OutFormatSupport)
        return E_FAIL;
      *pFormatSupport = support.OutFormatSupport;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(
        DXGI_FORMAT Format,
        UINT        SampleCount,
        UINT*       pNumQualityLevels) override {
      if (!pNumQualityLevels) return E_INVALIDARG;
      auto* backend = m_backendDevice.getBackend();
      *pNumQualityLevels = backend
        ? backend->checkMultisampleQualityLevels(Format, SampleCount)
        : ((SampleCount == 1) ? 1 : 0);
      return S_OK;
    }

    void STDMETHODCALLTYPE CheckCounterInfo(
        D3D11_COUNTER_INFO* pCounterInfo) override {
      if (pCounterInfo) {
        pCounterInfo->LastDeviceDependentCounter = static_cast<D3D11_COUNTER>(0);
        pCounterInfo->NumSimultaneousCounters    = 0;
        pCounterInfo->NumDetectableParallelUnits = 0;
      }
    }

    HRESULT STDMETHODCALLTYPE CheckCounter(
        const D3D11_COUNTER_DESC* pDesc,
        D3D11_COUNTER_TYPE*       pType,
        UINT*                     pActiveCounters,
        LPSTR                     szName,
        UINT*                     pNameLength,
        LPSTR                     szUnits,
        UINT*                     pUnitsLength,
        LPSTR                     szDescription,
        UINT*                     pDescriptionLength) override {
      // No device-dependent counters exist (see CheckCounterInfo).
      return DXGI_ERROR_UNSUPPORTED;
    }

    // The highest D3D11 feature level the D3D12 device has (11_1 to 12_1).
    D3D_FEATURE_LEVEL maxFeatureLevel() {
      static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1 };
      D3D12_FEATURE_DATA_FEATURE_LEVELS data = {};
      data.NumFeatureLevels = 3;
      data.pFeatureLevelsRequested = levels;
      ID3D12Device* device = nativeDevice();
      if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data))))
        return D3D_FEATURE_LEVEL_11_1;
      return (std::max)(data.MaxSupportedFeatureLevel, D3D_FEATURE_LEVEL_11_1);
    }

    // Before the device is handed out (D3D11CreateDevice).
    void setFeatureLevel(D3D_FEATURE_LEVEL level) { m_featureLevel = level; }

    D3D12_CONSERVATIVE_RASTERIZATION_TIER conservativeRasterizationTier() {
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      ID3D12Device* device = nativeDevice();
      if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        return D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
      return options.ConservativeRasterizationTier;
    }

    D3D11Multithread& GetMultithread() { return m_multithread; }

    // Diagnostics (dxup_frame_dump.txt): the immediate context logs the
    // state of every draw while this is set.
    std::atomic<bool> diagTraceDraws{false};
    std::atomic<uint32_t> diagTraceIndex{0};

    ID3D12Device* nativeDevice() {
      auto* backend = m_backendDevice.getBackend();
      return backend && backend->getBackendType() == BackendType::D3D12
        ? static_cast<d3d12::D3D12BackendDevice*>(backend)->getDevice()
        : nullptr;
    }

    // Answers from the D3D12 device where D3D11 and D3D12 share the meaning
    // (format support bits are identical). Features DXUP does not translate
    // are reported as unsupported, which the API allows for all of them.
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D11_FEATURE Feature,
        void*         pFeatureSupportData,
        UINT          FeatureSupportDataSize) override {
      if (!pFeatureSupportData)
        return E_INVALIDARG;

      ID3D12Device* device = nativeDevice();
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      if (device)
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));

      auto expectSize = [&](size_t size) { return FeatureSupportDataSize == size; };

      switch (Feature) {
        case D3D11_FEATURE_THREADING: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_THREADING)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_THREADING*>(pFeatureSupportData);
          data->DriverConcurrentCreates = TRUE;
          data->DriverCommandLists      = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_DOUBLES: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_DOUBLES)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_DOUBLES*>(pFeatureSupportData);
          data->DoublePrecisionFloatShaderOps = options.DoublePrecisionFloatShaderOps;
          return S_OK;
        }
        case D3D11_FEATURE_FORMAT_SUPPORT:
        case D3D11_FEATURE_FORMAT_SUPPORT2: {
          const bool second = Feature == D3D11_FEATURE_FORMAT_SUPPORT2;
          if (!expectSize(second ? sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT2) : sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT)))
            return E_INVALIDARG;
          // Both structures start with the format.
          const DXGI_FORMAT format = *static_cast<const DXGI_FORMAT*>(pFeatureSupportData);
          D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
          if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
            return E_FAIL;
          if (second) {
            // Like D3D11, fail when the format has no second-level support.
            if (!support.Support2)
              return E_FAIL;
            // Shared textures are not translated.
            UINT support2 = support.Support2 & ~UINT(D3D11_FORMAT_SUPPORT2_SHAREABLE);
            if (tiledResourcesTier() == D3D11_TILED_RESOURCES_NOT_SUPPORTED)
              support2 &= ~UINT(D3D11_FORMAT_SUPPORT2_TILED);
            static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2*>(pFeatureSupportData)->OutFormatSupport2 = support2;
          } else {
            UINT support1 = support.Support1;
            // Bits D3D12 does not report: every format can back staging
            // resources, and GenerateMips works for renderable, filterable
            // formats with mips.
            if (support1)
              support1 |= D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
            const UINT autogen = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_MIP;
            if ((support1 & autogen) == autogen)
              support1 |= D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
            static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT*>(pFeatureSupportData)->OutFormatSupport = support1;
          }
          return S_OK;
        }
        case D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS*>(pFeatureSupportData);
          data->ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS*>(pFeatureSupportData);
          std::memset(data, 0, sizeof(*data));
          data->OutputMergerLogicOp                    = options.OutputMergerLogicOp;
          data->UAVOnlyRenderingForcedSampleCount      = TRUE;
          data->DiscardAPIsSeenByDriver                = TRUE;
          data->FlagsForUpdateAndCopySeenByDriver      = TRUE;
          data->ClearView                              = TRUE;
          data->CopyWithOverlap                        = TRUE;
          data->ConstantBufferPartialUpdate            = TRUE;
          data->ConstantBufferOffsetting               = TRUE;
          data->MapNoOverwriteOnDynamicConstantBuffer  = TRUE;
          data->MapNoOverwriteOnDynamicBufferSRV       = TRUE;
          data->MultisampleRTVWithForcedSampleCountOne = FALSE;
          data->SAD4ShaderInstructions                 = TRUE;
          data->ExtendedDoublesShaderInstructions      = options.DoublePrecisionFloatShaderOps;
          data->ExtendedResourceSharing                = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_ARCHITECTURE_INFO: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_ARCHITECTURE_INFO)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_ARCHITECTURE_INFO*>(pFeatureSupportData)->TileBasedDeferredRenderer = FALSE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D9_OPTIONS: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D9_OPTIONS*>(pFeatureSupportData)->FullNonPow2TextureSupport = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT*>(pFeatureSupportData);
          data->PixelShaderMinPrecision          = UINT(options.MinPrecisionSupport);
          data->AllOtherShaderStagesMinPrecision = UINT(options.MinPrecisionSupport);
          return S_OK;
        }
        case D3D11_FEATURE_D3D9_SHADOW_SUPPORT: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT*>(pFeatureSupportData)
            ->SupportsDepthAsTextureWithLessEqualComparisonFilter = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS1: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS1*>(pFeatureSupportData);
          data->TiledResourcesTier                    = tiledResourcesTier();
          // D3D12 has min/max filtering from tiled resources tier 2.
          data->MinMaxFiltering                       = data->TiledResourcesTier >= D3D11_TILED_RESOURCES_TIER_2;
          data->ClearViewAlsoSupportsDepthOnlyFormats = FALSE;
          data->MapOnDefaultBuffers                   = FALSE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT*>(pFeatureSupportData)->SimpleInstancingSupported = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_MARKER_SUPPORT: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_MARKER_SUPPORT)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_MARKER_SUPPORT*>(pFeatureSupportData)->Profile = FALSE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D9_OPTIONS1: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_D3D9_OPTIONS1*>(pFeatureSupportData);
          data->FullNonPow2TextureSupported                                  = TRUE;
          data->DepthAsTextureWithLessEqualComparisonFilterSupported         = TRUE;
          data->SimpleInstancingSupported                                    = TRUE;
          data->TextureCubeFaceRenderTargetWithNonCubeDepthStencilSupported  = TRUE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS2: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2)))
            return E_INVALIDARG;
          D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
          if (device)
            device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture));
          auto* data = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS2*>(pFeatureSupportData);
          data->PSSpecifiedStencilRefSupported = options.PSSpecifiedStencilRefSupported;
          data->TypedUAVLoadAdditionalFormats  = options.TypedUAVLoadAdditionalFormats;
          data->ROVsSupported                  = options.ROVsSupported;
          // D3D11 and D3D12 number the tiers the same way.
          data->ConservativeRasterizationTier  = static_cast<D3D11_CONSERVATIVE_RASTERIZATION_TIER>(options.ConservativeRasterizationTier);
          data->TiledResourcesTier             = tiledResourcesTier();
          data->MapOnDefaultTextures           = FALSE;
          data->StandardSwizzle                = FALSE;
          data->UnifiedMemoryArchitecture      = architecture.UMA;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS3: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS3*>(pFeatureSupportData)->VPAndRTArrayIndexFromAnyShaderFeedingRasterizer =
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
          return S_OK;
        }
        case D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)))
            return E_INVALIDARG;
          D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va = {};
          if (device)
            device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof(va));
          auto* data = static_cast<D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*>(pFeatureSupportData);
          data->MaxGPUVirtualAddressBitsPerResource = va.MaxGPUVirtualAddressBitsPerResource;
          data->MaxGPUVirtualAddressBitsPerProcess  = va.MaxGPUVirtualAddressBitsPerProcess;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS4: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS4)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS4*>(pFeatureSupportData)->ExtendedNV12SharedTextureSupported = FALSE;
          return S_OK;
        }
        case D3D11_FEATURE_SHADER_CACHE: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_SHADER_CACHE)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_SHADER_CACHE*>(pFeatureSupportData)->SupportFlags = D3D11_SHADER_CACHE_SUPPORT_NONE;
          return S_OK;
        }
        case D3D11_FEATURE_D3D11_OPTIONS5: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS5)))
            return E_INVALIDARG;
          static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS5*>(pFeatureSupportData)->SharedResourceTier = D3D11_SHARED_RESOURCE_TIER_0;
          return S_OK;
        }
        case D3D11_FEATURE_DISPLAYABLE: {
          if (!expectSize(sizeof(D3D11_FEATURE_DATA_DISPLAYABLE)))
            return E_INVALIDARG;
          auto* data = static_cast<D3D11_FEATURE_DATA_DISPLAYABLE*>(pFeatureSupportData);
          data->DisplayableTexture = FALSE;
          data->SharedResourceTier = D3D11_SHARED_RESOURCE_TIER_0;
          return S_OK;
        }
        default:
          logMessage(LogLevel::Warn, "CheckFeatureSupport: unknown feature %d", static_cast<int>(Feature));
          return E_INVALIDARG;
      }
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID guid,
        UINT*   pDataSize,
        void*   pData) override {
      return m_privateData.getData(guid, pDataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID     guid,
        UINT        DataSize,
        const void* pData) override {
      return m_privateData.setData(guid, DataSize, pData);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID         guid,
        const IUnknown* pData) override {
      return m_privateData.setInterface(guid, pData);
    }

    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override {
      return m_featureLevel;
    }

    UINT STDMETHODCALLTYPE GetCreationFlags() override {
      return 0;
    }

    // Created for Direct3D 10 (d3d10.dll asks the device for ID3D10Device).
    void enableD3D10(UINT flags) {
      m_d3d10 = true;
      m_d3d10Device.setCreationFlags(flags);
    }

    // Why the device was lost, so that games notice a GPU fault and create
    // their device anew.
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
      ID3D12Device* device = nativeDevice();
      return device ? device->GetDeviceRemovedReason() : S_OK;
    }

    // Only D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR exists; DXUP raises no
    // exceptions for internal errors but keeps the mode as D3D11 does.
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override {
      if (RaiseFlags & ~UINT(D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR))
        return E_INVALIDARG;
      m_exceptionMode = RaiseFlags;
      return S_OK;
    }

    UINT STDMETHODCALLTYPE GetExceptionMode() override {
      return m_exceptionMode;
    }

    // ---- ID3D11On12Device ----

    HRESULT STDMETHODCALLTYPE CreateWrappedResource(
        IUnknown*                    pResource12,
        const D3D11_RESOURCE_FLAGS*  pFlags11,
        D3D12_RESOURCE_STATES        InState,
        D3D12_RESOURCE_STATES        OutState,
        REFIID                       riid,
        void**                       ppResource11) override {
      if (!pResource12 || !ppResource11)
        return E_INVALIDARG;

      *ppResource11 = nullptr;

      if (m_backendDevice.getBackendType() != BackendType::D3D12) {
        logMessage(LogLevel::Warn, "ID3D11On12Device::CreateWrappedResource unsupported backend type");
        return E_NOINTERFACE;
      }

      ComPtr<ID3D12Resource> resource12;
      HRESULT hr = pResource12->QueryInterface(
        __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(resource12.GetAddressOf()));
      if (FAILED(hr) || !resource12) {
        logMessage(LogLevel::Warn,
          "ID3D11On12Device::CreateWrappedResource failed to QI ID3D12Resource hr=0x%08x riid=%s",
          uint32_t(hr), formatGuid(riid).c_str());
        return FAILED(hr) ? hr : E_NOINTERFACE;
      }

      const D3D12_RESOURCE_DESC resourceDesc = resource12->GetDesc();
      logMessage(LogLevel::Debug,
        "ID3D11On12Device::CreateWrappedResource(dim=%u, riid=%s, inState=0x%x, outState=0x%x)",
        unsigned(resourceDesc.Dimension), formatGuid(riid).c_str(), unsigned(InState), unsigned(OutState));

      switch (resourceDesc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER: {
          D3D11_BUFFER_DESC bufferDesc = {};
          bufferDesc.ByteWidth = static_cast<UINT>(resourceDesc.Width);
          bufferDesc.Usage = D3D11_USAGE_DEFAULT;
          bufferDesc.BindFlags = getWrappedResourceBindFlags(resourceDesc, pFlags11);
          bufferDesc.CPUAccessFlags = pFlags11 ? pFlags11->CPUAccessFlags : 0;
          bufferDesc.MiscFlags = pFlags11 ? pFlags11->MiscFlags : 0;
          bufferDesc.StructureByteStride = pFlags11 ? pFlags11->StructureByteStride : 0;

          BufferDesc backendDesc = {};
          backendDesc.size = resourceDesc.Width;
          backendDesc.structStride = bufferDesc.StructureByteStride;
          backendDesc.usage = convertUsage(bufferDesc.Usage);
          backendDesc.bindFlags = convertBindFlags(bufferDesc.BindFlags);
          backendDesc.cpuAccess = convertCpuAccess(bufferDesc.CPUAccessFlags);
          backendDesc.miscFlags = bufferDesc.MiscFlags;

          auto backendBuffer = std::make_shared<d3d12::D3D12BackendBuffer>(std::move(resource12), backendDesc);
          backendBuffer->setCurrentState(InState);

          auto* buffer = new D3D11Buffer(this, bufferDesc, backendBuffer);
          hr = buffer->QueryInterface(riid, ppResource11);
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> resource11;
            if (SUCCEEDED(buffer->QueryInterface(
                  __uuidof(ID3D11Resource),
                  reinterpret_cast<void**>(resource11.GetAddressOf())))) {
              trackWrappedResourceState(resource11.Get(), InState, OutState);
            }
          }
          buffer->Release();
          return hr;
        }

        case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
          D3D11_TEXTURE1D_DESC textureDesc = {};
          textureDesc.Width = static_cast<UINT>(resourceDesc.Width);
          textureDesc.MipLevels = resourceDesc.MipLevels;
          textureDesc.ArraySize = resourceDesc.DepthOrArraySize;
          textureDesc.Format = resourceDesc.Format;
          textureDesc.Usage = D3D11_USAGE_DEFAULT;
          textureDesc.BindFlags = getWrappedResourceBindFlags(resourceDesc, pFlags11);
          textureDesc.CPUAccessFlags = pFlags11 ? pFlags11->CPUAccessFlags : 0;
          textureDesc.MiscFlags = pFlags11 ? pFlags11->MiscFlags : 0;

          ImageDesc backendDesc = {};
          backendDesc.type = ImageType::Tex1D;
          backendDesc.width = textureDesc.Width;
          backendDesc.height = 1;
          backendDesc.depth = 1;
          backendDesc.mipLevels = textureDesc.MipLevels;
          backendDesc.arraySize = textureDesc.ArraySize;
          backendDesc.format = textureDesc.Format;
          backendDesc.sampleCount = 1;
          backendDesc.sampleQuality = 0;
          backendDesc.usage = convertUsage(textureDesc.Usage);
          backendDesc.bindFlags = convertBindFlags(textureDesc.BindFlags);
          backendDesc.cpuAccess = convertCpuAccess(textureDesc.CPUAccessFlags);
          backendDesc.miscFlags = textureDesc.MiscFlags;

          auto backendImage = std::make_shared<d3d12::D3D12BackendImage>(std::move(resource12), backendDesc);
          backendImage->setCurrentState(InState);

          auto* texture = new D3D11Texture1D(this, textureDesc, backendImage);
          hr = texture->QueryInterface(riid, ppResource11);
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> resource11;
            if (SUCCEEDED(texture->QueryInterface(
                  __uuidof(ID3D11Resource),
                  reinterpret_cast<void**>(resource11.GetAddressOf())))) {
              trackWrappedResourceState(resource11.Get(), InState, OutState);
            }
          }
          texture->Release();
          return hr;
        }

        case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
          D3D11_TEXTURE2D_DESC textureDesc = {};
          textureDesc.Width = static_cast<UINT>(resourceDesc.Width);
          textureDesc.Height = resourceDesc.Height;
          textureDesc.MipLevels = resourceDesc.MipLevels;
          textureDesc.ArraySize = resourceDesc.DepthOrArraySize;
          textureDesc.Format = resourceDesc.Format;
          textureDesc.SampleDesc.Count = resourceDesc.SampleDesc.Count;
          textureDesc.SampleDesc.Quality = resourceDesc.SampleDesc.Quality;
          textureDesc.Usage = D3D11_USAGE_DEFAULT;
          textureDesc.BindFlags = getWrappedResourceBindFlags(resourceDesc, pFlags11);
          textureDesc.CPUAccessFlags = pFlags11 ? pFlags11->CPUAccessFlags : 0;
          textureDesc.MiscFlags = pFlags11 ? pFlags11->MiscFlags : 0;

          ImageDesc backendDesc = {};
          backendDesc.type = ImageType::Tex2D;
          backendDesc.width = textureDesc.Width;
          backendDesc.height = textureDesc.Height;
          backendDesc.depth = 1;
          backendDesc.mipLevels = textureDesc.MipLevels;
          backendDesc.arraySize = textureDesc.ArraySize;
          backendDesc.format = textureDesc.Format;
          backendDesc.sampleCount = textureDesc.SampleDesc.Count;
          backendDesc.sampleQuality = textureDesc.SampleDesc.Quality;
          backendDesc.usage = convertUsage(textureDesc.Usage);
          backendDesc.bindFlags = convertBindFlags(textureDesc.BindFlags);
          backendDesc.cpuAccess = convertCpuAccess(textureDesc.CPUAccessFlags);
          backendDesc.miscFlags = textureDesc.MiscFlags;

          auto backendImage = std::make_shared<d3d12::D3D12BackendImage>(std::move(resource12), backendDesc);
          backendImage->setCurrentState(InState);

          auto* texture = new D3D11Texture2D(this, textureDesc, backendImage);
          hr = texture->QueryInterface(riid, ppResource11);
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> resource11;
            if (SUCCEEDED(texture->QueryInterface(
                  __uuidof(ID3D11Resource),
                  reinterpret_cast<void**>(resource11.GetAddressOf())))) {
              trackWrappedResourceState(resource11.Get(), InState, OutState);
            }
          }
          texture->Release();
          return hr;
        }

        case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
          D3D11_TEXTURE3D_DESC textureDesc = {};
          textureDesc.Width = static_cast<UINT>(resourceDesc.Width);
          textureDesc.Height = resourceDesc.Height;
          textureDesc.Depth = resourceDesc.DepthOrArraySize;
          textureDesc.MipLevels = resourceDesc.MipLevels;
          textureDesc.Format = resourceDesc.Format;
          textureDesc.Usage = D3D11_USAGE_DEFAULT;
          textureDesc.BindFlags = getWrappedResourceBindFlags(resourceDesc, pFlags11);
          textureDesc.CPUAccessFlags = pFlags11 ? pFlags11->CPUAccessFlags : 0;
          textureDesc.MiscFlags = pFlags11 ? pFlags11->MiscFlags : 0;

          ImageDesc backendDesc = {};
          backendDesc.type = ImageType::Tex3D;
          backendDesc.width = textureDesc.Width;
          backendDesc.height = textureDesc.Height;
          backendDesc.depth = textureDesc.Depth;
          backendDesc.mipLevels = textureDesc.MipLevels;
          backendDesc.arraySize = 1;
          backendDesc.format = textureDesc.Format;
          backendDesc.sampleCount = 1;
          backendDesc.sampleQuality = 0;
          backendDesc.usage = convertUsage(textureDesc.Usage);
          backendDesc.bindFlags = convertBindFlags(textureDesc.BindFlags);
          backendDesc.cpuAccess = convertCpuAccess(textureDesc.CPUAccessFlags);
          backendDesc.miscFlags = textureDesc.MiscFlags;

          auto backendImage = std::make_shared<d3d12::D3D12BackendImage>(std::move(resource12), backendDesc);
          backendImage->setCurrentState(InState);

          auto* texture = new D3D11Texture3D(this, textureDesc, backendImage);
          hr = texture->QueryInterface(riid, ppResource11);
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> resource11;
            if (SUCCEEDED(texture->QueryInterface(
                  __uuidof(ID3D11Resource),
                  reinterpret_cast<void**>(resource11.GetAddressOf())))) {
              trackWrappedResourceState(resource11.Get(), InState, OutState);
            }
          }
          texture->Release();
          return hr;
        }

        default:
          logMessage(LogLevel::Warn,
            "ID3D11On12Device::CreateWrappedResource unsupported resource dimension=%u",
            unsigned(resourceDesc.Dimension));
          return E_NOINTERFACE;
      }
    }

    void STDMETHODCALLTYPE ReleaseWrappedResources(
        ID3D11Resource* const* ppResources,
        UINT                   NumResources) override {
      logMessage(LogLevel::Debug,
        "ID3D11On12Device::ReleaseWrappedResources(count=%u)", NumResources);

      for (UINT i = 0; i < NumResources; i++) {
        if (ppResources && ppResources[i])
          applyWrappedResourceState(ppResources[i], false);
      }

      ID3D11DeviceContext* immediateContext = nullptr;
      GetImmediateContext(&immediateContext);
      if (immediateContext) {
        immediateContext->Flush();
        immediateContext->Release();
      }
    }

    void STDMETHODCALLTYPE AcquireWrappedResources(
        ID3D11Resource* const* ppResources,
        UINT                   NumResources) override {
      logMessage(LogLevel::Debug,
        "ID3D11On12Device::AcquireWrappedResources(count=%u)", NumResources);

      for (UINT i = 0; i < NumResources; i++) {
        if (ppResources && ppResources[i])
          applyWrappedResourceState(ppResources[i], true);
      }
    }

    // ---- Internal Accessors ----

    D3D11BackendDevice* GetBackendDevice() { return &m_backendDevice; }
    IBackendDevice*     GetBackend()       { return m_backendDevice.getBackend(); }

    // Forced multisampling (dxup.conf d3d11.msaaForce) covers render targets
    // and depth buffers of the size of a swap chain: the scene, not shadow
    // maps or reduced buffers.
    void NoteBackBufferSize(UINT width, UINT height) {
      std::lock_guard<std::mutex> lock(m_backBufferSizeMutex);
      for (const auto& size : m_backBufferSizes) {
        if (size.first == width && size.second == height)
          return;
      }
      m_backBufferSizes.emplace_back(width, height);
    }

    bool IsBackBufferSize(UINT width, UINT height) {
      std::lock_guard<std::mutex> lock(m_backBufferSizeMutex);
      for (const auto& size : m_backBufferSizes) {
        if (size.first == width && size.second == height)
          return true;
      }
      return false;
    }

    // Whether any texture has a forced multisampled copy; contexts track
    // nothing until then.
    bool HasForcedMsaa() const { return m_forcedMsaa.load(std::memory_order_relaxed); }

    // The formats a single-sampled texture is multisampled with, or false
    // when it is not.
    bool ShouldForceMsaa(const D3D11_TEXTURE2D_DESC& desc, ForcedMsaaFormats& formats) {
      const UserOverrides& overrides = UserOverrides::get();
      const UINT excludedMisc = kResourceMiscShared | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
        | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_TEXTURECUBE
        | D3D11_RESOURCE_MISC_GDI_COMPATIBLE | D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_GENERATE_MIPS;
      const bool depth = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
      if (!overrides.msaaForce || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1
       || desc.Usage != D3D11_USAGE_DEFAULT || !(desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))
       || (desc.MiscFlags & excludedMisc) || !getForcedMsaaFormats(desc.Format, depth, formats)
       || !IsBackBufferSize(desc.Width, desc.Height))
        return false;
      UINT levels = 0;
      return SUCCEEDED(CheckMultisampleQualityLevels(formats.typed, static_cast<UINT>(overrides.msaaSamples), &levels))
        && levels;
    }

    // Gives a texture its multisampled copy.
    void AttachMsaaShadow(D3D11Texture2D* texture, const ForcedMsaaFormats& formats) {
      IBackendImage* image = texture->GetBackendImage();
      auto* backend = GetBackend();
      if (!image || !backend)
        return;
      ImageDesc shadowDesc = image->getDesc();
      shadowDesc.sampleCount = static_cast<uint32_t>(UserOverrides::get().msaaSamples);
      shadowDesc.sampleQuality = 0;
      shadowDesc.mipLevels = 1;
      // Depth is resolved by a shader that reads it.
      shadowDesc.bindFlags = formats.depth
        ? BindFlags::DepthStencil | BindFlags::ShaderResource
        : BindFlags::RenderTarget;
      shadowDesc.miscFlags = 0;
      auto shadow = backend->createImage(shadowDesc);
      if (!shadow) {
        logMessage(LogLevel::Warn, "forced MSAA: no %ux multisampled copy for a %ux%u texture of format %u",
          shadowDesc.sampleCount, shadowDesc.width, shadowDesc.height, static_cast<uint32_t>(shadowDesc.format));
        return;
      }
      texture->SetMsaaShadow(std::move(shadow), formats);
      m_forcedMsaa.store(true, std::memory_order_relaxed);
    }

    // The sample count DXUP renders a multisampled texture of the
    // application with: dxup.conf can raise it where the format supports
    // that, never lower it. The application keeps seeing its own count.
    UINT EnhancedSampleCount(DXGI_FORMAT format, UINT appCount) {
      const int32_t forced = UserOverrides::get().msaaSamples;
      if (appCount <= 1 || forced <= static_cast<int32_t>(appCount))
        return appCount;
      UINT levels = 0;
      if (FAILED(CheckMultisampleQualityLevels(format, static_cast<UINT>(forced), &levels)) || !levels)
        return appCount;
      return static_cast<UINT>(forced);
    }

    // IDXGIDevice1 maximum frame latency as set by the application; 0 means
    // the default of 3. The swap chain Present enforces the effective value,
    // which dxup.conf can force.
    UINT GetMaximumFrameLatency() const { return m_maxFrameLatency.load(std::memory_order_relaxed); }
    void SetMaximumFrameLatency(UINT latency) { m_maxFrameLatency.store(latency, std::memory_order_relaxed); }
    UINT GetEffectiveFrameLatency() const {
      const int32_t forced = UserOverrides::get().maxFrameLatency;
      if (forced > 0)
        return static_cast<UINT>(forced);
      const UINT latency = GetMaximumFrameLatency();
      return latency ? latency : 3u;
    }
    BackendType         GetBackendType() const { return m_backendDevice.getBackendType(); }

  private:
    struct WrappedResourceState {
      D3D12_RESOURCE_STATES inState;
      D3D12_RESOURCE_STATES outState;
    };

    void trackWrappedResourceState(
        ID3D11Resource*        resource,
        D3D12_RESOURCE_STATES  inState,
        D3D12_RESOURCE_STATES  outState) {
      if (!resource)
        return;

      std::lock_guard<std::mutex> lock(m_wrappedResourceMutex);
      m_wrappedResourceStates[resource] = { inState, outState };
    }

    void applyWrappedResourceState(ID3D11Resource* resource, bool acquire) {
      if (!resource || m_backendDevice.getBackendType() != BackendType::D3D12)
        return;

      WrappedResourceState state = {};
      {
        std::lock_guard<std::mutex> lock(m_wrappedResourceMutex);
        auto entry = m_wrappedResourceStates.find(resource);
        if (entry == m_wrappedResourceStates.end())
          return;
        state = entry->second;
      }

      D3D12_RESOURCE_STATES targetState = acquire ? state.inState : state.outState;
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);

      switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
          auto* buffer = static_cast<D3D11Buffer*>(resource);
          auto* backendBuffer = static_cast<d3d12::D3D12BackendBuffer*>(buffer->GetBackendBuffer());
          if (backendBuffer)
            backendBuffer->setCurrentState(targetState);
          break;
        }

        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
          auto* texture = static_cast<D3D11Texture1D*>(resource);
          auto* backendImage = static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage());
          if (backendImage)
            backendImage->setCurrentState(targetState);
          break;
        }

        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
          auto* texture = static_cast<D3D11Texture2D*>(resource);
          auto* backendImage = static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage());
          if (backendImage)
            backendImage->setCurrentState(targetState);
          break;
        }

        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
          auto* texture = static_cast<D3D11Texture3D*>(resource);
          auto* backendImage = static_cast<d3d12::D3D12BackendImage*>(texture->GetBackendImage());
          if (backendImage)
            backendImage->setCurrentState(targetState);
          break;
        }

        default:
          break;
      }
    }

    std::atomic<ULONG>       m_refCount;
    D3D11BackendDevice       m_backendDevice;
    UINT                     m_exceptionMode = 0;
    std::atomic<uint32_t>    m_sharedResources{0};
    // Format support per format, with bit 32 set once known.
    std::array<std::atomic<uint64_t>, 256> m_formatSupportCache = {};
    D3D_FEATURE_LEVEL        m_featureLevel;
    std::mutex               m_samplerCacheMutex;
    std::unordered_map<std::string, D3D11SamplerState*> m_samplerCache;
    D3D11Multithread         m_multithread{ reinterpret_cast<IUnknown*>(static_cast<ID3D11Device5*>(this)) };
    // ID3D10Device1, for devices Windows' d3d10.dll creates.
    D3D10Device              m_d3d10Device{ static_cast<ID3D11Device5*>(this) };
    bool                     m_d3d10 = false;
    std::mutex               m_removedEventMutex;
    DWORD                    m_removedEventCookie = 0;
    std::vector<std::pair<DWORD, ComPtr<ID3D12Fence>>> m_removedEventFences;
    D3D11ImmediateContext*   m_immediateContext = nullptr;
    DXGIDevice*              m_dxgiDevice = nullptr;
    std::mutex               m_dxgiDeviceMutex;
    std::mutex               m_wrappedResourceMutex;
    std::unordered_map<ID3D11Resource*, WrappedResourceState> m_wrappedResourceStates;
    // Shared with the IDXGIDevice returned by QueryInterface, which
    // forwards its IDXGIObject private data methods here.
    ComPrivateData           m_privateData;
    std::atomic<UINT>        m_maxFrameLatency{3};
    std::mutex               m_backBufferSizeMutex;
    std::vector<std::pair<UINT, UINT>> m_backBufferSizes;
    std::atomic<bool>        m_forcedMsaa{false};
  };

  // ============================================================
  // D3D11DeviceChild::GetDevice implementation
  // (defined after D3D11Device is complete)
  // ============================================================

  inline void D3D11CommandList::GetDevice(ID3D11Device** ppDevice) {
    if (ppDevice) {
      *ppDevice = reinterpret_cast<ID3D11Device*>(m_device);
      if (m_device) reinterpret_cast<ID3D11Device*>(m_device)->AddRef();
    }
  }

  template<typename Base>
  void D3D11DeviceChild<Base>::GetDevice(ID3D11Device** ppDevice) {
    if (ppDevice) {
      *ppDevice = m_device;
      if (m_device) m_device->AddRef();
    }
  }

  template<typename Base>
  D3D11DeviceChild<Base>::D3D11DeviceChild(D3D11Device* pDevice)
    : m_refCount(1), m_refPrivate(1), m_device(pDevice) {
    acquireDevice(m_device);
  }

  template<typename Base>
  void D3D11DeviceChild<Base>::acquireDevice(D3D11Device* device) {
    if (device)
      device->AddRef();
  }

  template<typename Base>
  void D3D11DeviceChild<Base>::releaseDevice(D3D11Device* device) {
    if (device)
      device->Release();
  }

  inline D3D11SamplerState::~D3D11SamplerState() {
    if (m_device)
      m_device->forgetSamplerState(m_desc, this);
  }

  inline void D3D11Buffer::allocateDeferredBuffer() const {
    // Runs once, under m_allocationOnce, before any reader sees m_buffer.
    auto buffer = m_device->GetBackendDevice()->createBuffer(&m_desc, nullptr);
    if (!buffer) {
      logMessage(LogLevel::Error, "Deferred staging buffer allocation failed (size=%u)", m_desc.ByteWidth);
      return;
    }
    const_cast<D3D11Buffer*>(this)->m_buffer = std::move(buffer);
    const_cast<D3D11Buffer*>(this)->setBindData(m_buffer.get(), nullptr);
  }

  inline RecordedMsaaUse::RecordedMsaaUse(D3D11Texture2D* texture, MsaaUse first)
  : m_texture(texture), m_first(first), m_write(first != MsaaUse::Read) {
    m_texture->AddRefPrivate();
  }

  inline RecordedMsaaUse& RecordedMsaaUse::operator=(RecordedMsaaUse&& other) noexcept {
    if (this != &other) {
      if (m_texture)
        m_texture->ReleasePrivate();
      m_texture = other.m_texture;
      m_first = other.m_first;
      m_write = other.m_write;
      other.m_texture = nullptr;
    }
    return *this;
  }

  inline RecordedMsaaUse::~RecordedMsaaUse() {
    if (m_texture)
      m_texture->ReleasePrivate();
  }

  inline D3D11CommandList::D3D11CommandList(D3D11Device* pDevice, Rc<IBackendCommandList> commands,
                                            std::vector<RecordedDiscard> discards,
                                            std::vector<RecordedMsaaUse> msaaUses)
    : m_refCount(1)
    , m_device(pDevice)
    , m_commands(std::move(commands))
    , m_discards(std::move(discards))
    , m_msaaUses(std::move(msaaUses)) {
    if (m_device)
      m_device->AddRef();
  }

  inline D3D11CommandList::~D3D11CommandList() {
    // Drop the recorded commands before the device that owns their backend.
    m_commands = nullptr;
    m_discards.clear();
    m_msaaUses.clear();
    if (m_device)
      m_device->Release();
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetParent(REFIID riid, void** ppParent) {
    return m_device ? m_device->QueryInterface(riid, ppParent) : E_NOINTERFACE;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetDevice(REFIID riid, void** ppDevice) {
    return m_device ? m_device->QueryInterface(riid, ppDevice) : E_NOINTERFACE;
  }

  // Resources shared without NT handles have a legacy handle, the same
  // every time.
  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetSharedHandle(HANDLE* pSharedHandle) {
    if (!pSharedHandle)
      return E_INVALIDARG;
    *pSharedHandle = nullptr;
    if (m_separate) {
      ComPtr<IDXGIResource> owner;
      return SUCCEEDED(m_resource->QueryInterface(IID_PPV_ARGS(&owner)))
        ? owner->GetSharedHandle(pSharedHandle) : E_INVALIDARG;
    }
    if (!m_shared || (m_shared->miscFlags() & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;
    ID3D12Device* device = m_device->nativeDevice();
    ID3D12Resource* resource = m_device->nativeResource(m_resource);
    if (!device || !resource)
      return E_INVALIDARG;
    const HANDLE handle = m_shared->kmtHandle([&] {
      HANDLE created = nullptr;
      if (FAILED(device->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &created)))
        created = nullptr;
      return created;
    });
    if (!handle)
      return E_FAIL;
    *pSharedHandle = handle;
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::CreateSharedHandle(
      const SECURITY_ATTRIBUTES* pAttributes, DWORD access, LPCWSTR name, HANDLE* pHandle) {
    if (!pHandle)
      return E_INVALIDARG;
    *pHandle = nullptr;
    if (m_separate) {
      ComPtr<IDXGIResource1> owner;
      return SUCCEEDED(m_resource->QueryInterface(IID_PPV_ARGS(&owner)))
        ? owner->CreateSharedHandle(pAttributes, access, name, pHandle) : E_INVALIDARG;
    }
    if (!m_shared || !(m_shared->miscFlags() & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;
    ID3D12Device* device = m_device->nativeDevice();
    ID3D12Resource* resource = m_device->nativeResource(m_resource);
    if (!device || !resource)
      return E_INVALIDARG;
    // D3D12 handles carry all access rights.
    HANDLE handle = nullptr;
    const HRESULT hr = device->CreateSharedHandle(resource, pAttributes, GENERIC_ALL, name, &handle);
    if (FAILED(hr))
      return name && (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        ? DXGI_ERROR_NAME_ALREADY_EXISTS : hr;
    m_shared->addHandle(handle);
    *pHandle = handle;
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::AcquireSync(UINT64 key, DWORD milliseconds) {
    if (!m_shared || !m_shared->hasKeyedMutex())
      return E_FAIL;
    D3D11SharedResource& shared = *m_shared;
    std::unique_lock<std::mutex> lock(shared.keyMutex);
    // Acquiring again before releasing would wait for itself.
    if (shared.keyHeld && shared.keyOwner == this && !shared.keyReleasePending)
      return E_FAIL;
    auto ready = [&] { return shared.keyAbandoned || (!shared.keyHeld && shared.key == key); };
    if (milliseconds == INFINITE)
      shared.keyChanged.wait(lock, ready);
    else if (!shared.keyChanged.wait_for(lock, std::chrono::milliseconds(milliseconds), ready))
      return static_cast<HRESULT>(WAIT_TIMEOUT);
    if (shared.keyAbandoned)
      return static_cast<HRESULT>(WAIT_ABANDONED);
    shared.keyHeld = true;
    shared.keyOwner = this;
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::ReleaseSync(UINT64 key) {
    if (!m_shared || !m_shared->hasKeyedMutex())
      return E_FAIL;
    std::shared_ptr<D3D11SharedResource> shared = m_shared;
    {
      std::lock_guard<std::mutex> lock(shared->keyMutex);
      if (!shared->keyHeld || shared->keyOwner != this || shared->keyReleasePending)
        return E_FAIL;
      shared->keyReleasePending = true;
    }
    // The next owner proceeds once the work recorded so far is submitted;
    // its own work then waits for that submission.
    m_device->submitThen([shared, key] {
      std::lock_guard<std::mutex> lock(shared->keyMutex);
      shared->keyHeld = false;
      shared->keyReleasePending = false;
      shared->keyOwner = nullptr;
      shared->key = key;
      shared->keyChanged.notify_all();
    });
    return S_OK;
  }

  inline void D3D11DxgiResource::setShared(std::shared_ptr<D3D11SharedResource> shared) {
    if (shared && !m_shared)
      m_device->sharedResourceAdded();
    m_shared = std::move(shared);
  }

  // A keyed mutex this resource holds when destroyed is abandoned.
  inline void D3D11DxgiResource::releaseShared() {
    if (!m_shared)
      return;
    m_device->sharedResourceRemoved();
    if (!m_shared->hasKeyedMutex())
      return;
    std::lock_guard<std::mutex> lock(m_shared->keyMutex);
    if (m_shared->keyHeld && m_shared->keyOwner == this && !m_shared->keyReleasePending) {
      m_shared->keyHeld = false;
      m_shared->keyOwner = nullptr;
      m_shared->keyAbandoned = true;
      m_shared->keyChanged.notify_all();
    }
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetUsage(DXGI_USAGE* pUsage) {
    if (!pUsage)
      return E_INVALIDARG;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    m_resource->GetType(&dim);
    UINT bindFlags = 0;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    switch (dim) {
      case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC desc = {};
        static_cast<ID3D11Buffer*>(m_resource)->GetDesc(&desc);
        bindFlags = desc.BindFlags;
        usage = desc.Usage;
        break;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC desc = {};
        static_cast<ID3D11Texture1D*>(m_resource)->GetDesc(&desc);
        bindFlags = desc.BindFlags;
        usage = desc.Usage;
        break;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC desc = {};
        static_cast<ID3D11Texture2D*>(m_resource)->GetDesc(&desc);
        bindFlags = desc.BindFlags;
        usage = desc.Usage;
        break;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC desc = {};
        static_cast<ID3D11Texture3D*>(m_resource)->GetDesc(&desc);
        bindFlags = desc.BindFlags;
        usage = desc.Usage;
        break;
      }
      default:
        break;
    }
    DXGI_USAGE result = 0;
    if (bindFlags & D3D11_BIND_SHADER_RESOURCE)
      result |= DXGI_USAGE_SHADER_INPUT;
    if (bindFlags & D3D11_BIND_RENDER_TARGET)
      result |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (bindFlags & D3D11_BIND_UNORDERED_ACCESS)
      result |= DXGI_USAGE_UNORDERED_ACCESS;
    switch (usage) {
      case D3D11_USAGE_DYNAMIC: result |= DXGI_CPU_ACCESS_DYNAMIC; break;
      case D3D11_USAGE_STAGING: result |= DXGI_CPU_ACCESS_READ_WRITE; break;
      default: result |= DXGI_CPU_ACCESS_NONE; break;
    }
    // A swap chain's buffers say so, as with DXGI.
    UINT marker = 0, markerSize = sizeof(marker);
    if (SUCCEEDED(m_resource->GetPrivateData(kDxupSwapChainBufferGuid, &markerSize, &marker)) && marker)
      result |= DXGI_USAGE_BACK_BUFFER;
    *pUsage = result;
    return S_OK;
  }

  inline bool D3D11DxgiResource::subresourceExtent(D3D11_TEXTURE2D_DESC& desc, UINT& width, UINT& height) const {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    m_resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return false;
    static_cast<ID3D11Texture2D*>(m_resource)->GetDesc(&desc);
    const UINT mips = desc.MipLevels ? desc.MipLevels : 1u;
    if (m_subresource >= mips * desc.ArraySize)
      return false;
    const UINT mip = m_subresource % mips;
    width = (std::max)(desc.Width >> mip, 1u);
    height = (std::max)(desc.Height >> mip, 1u);
    return true;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::CreateSubresourceSurface(UINT index, IDXGISurface2** ppSurface) {
    if (!ppSurface)
      return E_INVALIDARG;
    *ppSurface = nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    m_resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc = {};
    static_cast<ID3D11Texture2D*>(m_resource)->GetDesc(&desc);
    if (index >= (desc.MipLevels ? desc.MipLevels : 1u) * desc.ArraySize)
      return E_INVALIDARG;
    *ppSurface = new D3D11DxgiResource(m_resource, m_device, index, true);
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetDesc(DXGI_SURFACE_DESC* pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    m_resource->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC bufferDesc = {};
      static_cast<ID3D11Buffer*>(m_resource)->GetDesc(&bufferDesc);
      *pDesc = { bufferDesc.ByteWidth, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 } };
      return S_OK;
    }
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
      D3D11_TEXTURE1D_DESC textureDesc = {};
      static_cast<ID3D11Texture1D*>(m_resource)->GetDesc(&textureDesc);
      *pDesc = { textureDesc.Width, 1, textureDesc.Format, { 1, 0 } };
      return S_OK;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    UINT width = 0, height = 0;
    if (!subresourceExtent(desc, width, height))
      return E_INVALIDARG;
    pDesc->Width = width;
    pDesc->Height = height;
    pDesc->Format = desc.Format;
    pDesc->SampleDesc = desc.SampleDesc;
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::Map(DXGI_MAPPED_RECT* pLockedRect, UINT flags) {
    if (!pLockedRect || !m_device)
      return E_INVALIDARG;
    D3D11_MAP type = D3D11_MAP_WRITE;
    if (flags & DXGI_MAP_DISCARD)
      type = D3D11_MAP_WRITE_DISCARD;
    else if ((flags & DXGI_MAP_READ) && (flags & DXGI_MAP_WRITE))
      type = D3D11_MAP_READ_WRITE;
    else if (flags & DXGI_MAP_READ)
      type = D3D11_MAP_READ;
    ComPtr<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(context.GetAddressOf());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(m_resource, m_subresource, type, 0, &mapped);
    if (FAILED(hr))
      return hr;
    pLockedRect->Pitch = static_cast<INT>(mapped.RowPitch);
    pLockedRect->pBits = static_cast<BYTE*>(mapped.pData);
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::Unmap() {
    if (!m_device)
      return E_INVALIDARG;
    ComPtr<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(context.GetAddressOf());
    context->Unmap(m_resource, m_subresource);
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::GetDC(BOOL discard, HDC* phdc) {
    if (!phdc)
      return E_INVALIDARG;
    *phdc = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    UINT width = 0, height = 0;
    if (!subresourceExtent(desc, width, height) || !(desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) || m_dc)
      return DXGI_ERROR_INVALID_CALL;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LONG(width);
    info.bmiHeader.biHeight = -LONG(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    m_bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    m_dc = m_bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!m_dc) {
      releaseDC();
      return E_OUTOFMEMORY;
    }
    m_previousBitmap = SelectObject(m_dc, m_bitmap);

    // GDI draws over the current contents.
    if (!discard) {
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Width = width;
      stagingDesc.Height = height;
      stagingDesc.MipLevels = 1;
      stagingDesc.ArraySize = 1;
      stagingDesc.SampleDesc = { 1, 0 };
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.MiscFlags = 0;
      ComPtr<ID3D11Texture2D> staging;
      ComPtr<ID3D11DeviceContext> context;
      m_device->GetImmediateContext(context.GetAddressOf());
      if (SUCCEEDED(m_device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf()))) {
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, m_resource, m_subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
          for (UINT y = 0; y < height; y++)
            std::memcpy(static_cast<uint8_t*>(m_bits) + size_t(y) * width * 4,
              static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(width) * 4);
          context->Unmap(staging.Get(), 0);
        }
      }
    }
    *phdc = m_dc;
    return S_OK;
  }

  inline HRESULT STDMETHODCALLTYPE D3D11DxgiResource::ReleaseDC(RECT* pDirtyRect) {
    if (!m_dc)
      return DXGI_ERROR_INVALID_CALL;
    D3D11_TEXTURE2D_DESC desc = {};
    UINT width = 0, height = 0;
    if (subresourceExtent(desc, width, height)) {
      GdiFlush();
      D3D11_BOX box = { 0, 0, 0, width, height, 1 };
      if (pDirtyRect) {
        box.left = UINT((std::max)(pDirtyRect->left, 0L));
        box.top = UINT((std::max)(pDirtyRect->top, 0L));
        box.right = (std::min)(UINT((std::max)(pDirtyRect->right, 0L)), width);
        box.bottom = (std::min)(UINT((std::max)(pDirtyRect->bottom, 0L)), height);
      }
      if (box.left < box.right && box.top < box.bottom) {
        ComPtr<ID3D11DeviceContext> context;
        m_device->GetImmediateContext(context.GetAddressOf());
        const auto* source = static_cast<const uint8_t*>(m_bits) + size_t(box.top) * width * 4 + size_t(box.left) * 4;
        context->UpdateSubresource(m_resource, m_subresource, &box, source, width * 4, 0);
      }
    }
    releaseDC();
    return S_OK;
  }

  inline void D3D11DxgiResource::releaseDC() {
    if (m_dc) {
      SelectObject(m_dc, m_previousBitmap);
      DeleteDC(m_dc);
      m_dc = nullptr;
    }
    if (m_bitmap) {
      DeleteObject(m_bitmap);
      m_bitmap = nullptr;
    }
    m_bits = nullptr;
    m_previousBitmap = nullptr;
  }

} // namespace dxup

#endif // _WIN32
