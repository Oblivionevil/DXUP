#pragma once

// The tools of the draw trace (dxup_trace_go.txt): textures as images before
// the calls dxup_dump_calls.txt lists, and pixels at dxup_probe_pixel.txt's
// position in the trace's lines. probing: the calls these make are not traced.

#include "d3d11_d3d12_bridge.h"
#include "../util/com_ptr.h"
#include "../util/util_paths.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dxup {

  template <typename... Args>
  std::string formatTrace(const char* format, Args... args) {
    char text[512];
    sprintf_s(text, format, args...);
    return text;
  }

  inline const std::vector<uint32_t>& traceCallsToDump() {
    static const std::vector<uint32_t> calls = [] {
      std::vector<uint32_t> list;
      if (FILE* file = dxupOpenSibling("dxup_dump_calls.txt", "r")) {
        unsigned value = 0;
        while (fscanf_s(file, "%u", &value) == 1)
          list.push_back(value);
        fclose(file);
      }
      return list;
    }();
    return calls;
  }

  // "x y" in 3840x2160 coordinates; negative: none.
  inline POINT traceProbePixel() {
    static const POINT probe = [] {
      POINT point = { -1, -1 };
      if (FILE* file = dxupOpenSibling("dxup_probe_pixel.txt", "r")) {
        if (fscanf_s(file, "%ld %ld", &point.x, &point.y) != 2)
          point = { -1, -1 };
        fclose(file);
      }
      return point;
    }();
    return probe;
  }

  inline float traceHalfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000) << 16;
    const uint32_t exponent = (half >> 10) & 0x1f;
    const uint32_t mantissa = half & 0x3ff;
    if (!exponent)
      return (half & 0x8000 ? -1.0f : 1.0f) * float(mantissa) * (1.0f / 16777216.0f);
    const uint32_t bits = exponent == 31
      ? sign | 0x7f800000 | (mantissa << 13)
      : sign | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }

  inline void dumpTraceTexture(ID3D11Device* device, ID3D11DeviceContext* context, bool& probing, ID3D11Resource* resource, const char* name) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
    D3D11_TEXTURE2D_DESC desc = {};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.Width < 256)
      return;
    const uint32_t texelBytes = getFormatBpp(desc.Format);
    if (texelBytes != 4 && texelBytes != 8)
      return;
    D3D11_TEXTURE2D_DESC stagingDesc = { desc.Width, desc.Height, 1, 1, desc.Format, { 1, 0 },
      D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
      return;
    probing = true;
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      if (FILE* file = dxupOpenSibling(name, "wb")) {
        const uint32_t imageSize = desc.Width * desc.Height * 4;
        BITMAPFILEHEADER fileHeader = {};
        BITMAPINFOHEADER infoHeader = {};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = LONG(desc.Width);
        infoHeader.biHeight = -LONG(desc.Height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biSizeImage = imageSize;
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&infoHeader, sizeof(infoHeader), 1, file);
        auto toByte = [](float value) {
          value = value != value ? 0.0f : (std::min)((std::max)(value, 0.0f), 1.0f);
          return uint8_t(std::pow(value, 1.0f / 2.2f) * 255.0f + 0.5f);
        };
        std::vector<uint8_t> row(desc.Width * 4);
        for (UINT y = 0; y < desc.Height; y++) {
          const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
          for (UINT x = 0; x < desc.Width; x++) {
            float rgb[3] = {};
            if (texelBytes == 8) {
              const uint16_t* halves = reinterpret_cast<const uint16_t*>(src + x * 8);
              const bool unorm = desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM;
              for (int c = 0; c < 3; c++)
                rgb[c] = unorm ? halves[c] / 65535.0f : traceHalfToFloat(halves[c]);
            } else {
              uint32_t word = 0;
              std::memcpy(&word, src + x * 4, 4);
              switch (desc.Format) {
                case DXGI_FORMAT_R11G11B10_FLOAT:
                  // Small floats moved into the half layout.
                  rgb[0] = traceHalfToFloat(uint16_t((word & 0x7ff) << 4));
                  rgb[1] = traceHalfToFloat(uint16_t(((word >> 11) & 0x7ff) << 4));
                  rgb[2] = traceHalfToFloat(uint16_t(((word >> 22) & 0x3ff) << 5));
                  break;
                case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
                  rgb[0] = traceHalfToFloat(uint16_t(word));
                  rgb[1] = traceHalfToFloat(uint16_t(word >> 16));
                  break;
                case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: {
                  float value;
                  std::memcpy(&value, &word, 4);
                  rgb[0] = rgb[1] = rgb[2] = value;
                  break;
                }
                case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
                  rgb[0] = (word & 0x3ff) / 1023.0f;
                  rgb[1] = ((word >> 10) & 0x3ff) / 1023.0f;
                  rgb[2] = ((word >> 20) & 0x3ff) / 1023.0f;
                  break;
                default:
                  // 8-bit channels: the stored values as they are.
                  for (int c = 0; c < 3; c++)
                    rgb[c] = std::pow(((word >> (c * 8)) & 0xff) / 255.0f, 2.2f);
                  break;
              }
            }
            row[x * 4 + 0] = toByte(rgb[2]);
            row[x * 4 + 1] = toByte(rgb[1]);
            row[x * 4 + 2] = toByte(rgb[0]);
            row[x * 4 + 3] = 255;
          }
          fwrite(row.data(), row.size(), 1, file);
        }
        fclose(file);
      }
      context->Unmap(staging.Get(), 0);
    }
    probing = false;
  }

  inline void probeTraceMips(ID3D11Device* device, ID3D11DeviceContext* context, bool& probing, std::string& line, ID3D11Resource* resource) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
    D3D11_TEXTURE2D_DESC desc = {};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    if (desc.Width > 512 || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
      return;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
      return;
    probing = true;
    context->CopyResource(staging.Get(), resource);
    line += " mips(";
    for (UINT mip = 0; mip < desc.MipLevels; mip++) {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (FAILED(context->Map(staging.Get(), mip, D3D11_MAP_READ, 0, &mapped)))
        continue;
      const UINT rows = (std::max)(1u, (desc.Height >> mip) / 4u);
      uint64_t sum = 0, nonzero = 0;
      for (UINT row = 0; row < rows; row++) {
        const uint8_t* bytes = static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        const UINT rowBytes = (std::min)(mapped.RowPitch, UINT(((std::max)(1u, (desc.Width >> mip) / 4u)) * 16u));
        for (UINT i = 0; i < rowBytes; i++) {
          sum += bytes[i];
          nonzero += bytes[i] != 0;
        }
      }
      line += formatTrace("%s%u:%llu/%llu", mip ? " " : "", mip, static_cast<unsigned long long>(nonzero),
        static_cast<unsigned long long>(sum));
      context->Unmap(staging.Get(), mip);
    }
    line += ")";
    probing = false;
  }

  inline void probeTraceTexture(ID3D11Device* device, ID3D11DeviceContext* context, bool& probing, std::string& line, const char* name, ID3D11Resource* resource, POINT probe, bool fullResolutionOnly) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
    D3D11_TEXTURE2D_DESC desc = {};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || (fullResolutionOnly && desc.Width != 3840))
      return;
    const UINT x = UINT(probe.x) * desc.Width / 3840, y = UINT(probe.y) * desc.Height / 2160;
    D3D11_TEXTURE2D_DESC stagingDesc = { 1, 1, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
      return;
    probing = true;
    const D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      const auto* words = static_cast<const uint32_t*>(mapped.pData);
      line += formatTrace("%s(%p@%u,%u=%08x %08x)", name, static_cast<void*>(resource), x, y, words[0], words[1]);
      context->Unmap(staging.Get(), 0);
    }
    probing = false;
  }

}
